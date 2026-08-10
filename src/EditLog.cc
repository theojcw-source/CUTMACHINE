#include "EditLog.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

class LogReader {
public:
    explicit LogReader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string Object() {
        Skip();
        if (position_ >= input_.size() || input_[position_] != '{') {
            throw std::runtime_error("expected operation object at byte " +
                                     std::to_string(position_));
        }
        const size_t start = position_;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (inString) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    inString = false;
                continue;
            }
            if (character == '"')
                inString = true;
            else if (character == '{')
                ++depth;
            else if (character == '}' && --depth == 0)
                return input_.substr(start, position_ - start);
        }
        throw std::runtime_error("unterminated operation object");
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing edit log JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

void WriteEntries(std::ostringstream& output,
                  const std::vector<Entry>& entries) {
    output << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index) output << ',';
        output << "{\"op\":" << SerializeOperation(entries[index].op)
               << ",\"inverse\":" << SerializeOperation(entries[index].inverse)
               << '}';
    }
    output << ']';
}

std::vector<Entry> ReadEntries(LogReader& reader) {
    std::vector<Entry> entries;
    reader.Expect("[");
    if (reader.Consume("]")) return entries;
    while (true) {
        reader.Expect("{\"op\":");
        const std::string operationJson = reader.Object();
        reader.Expect(",\"inverse\":");
        const std::string inverseJson = reader.Object();
        reader.Expect("}");
        Entry entry;
        EditError error = EditError::None;
        std::string message;
        if (!DeserializeOperation(operationJson, entry.op, error, message))
            throw std::runtime_error("invalid logged operation: " + message);
        if (!DeserializeOperation(inverseJson, entry.inverse, error, message))
            throw std::runtime_error("invalid logged inverse: " + message);
        entries.push_back(std::move(entry));
        if (reader.Consume("]")) return entries;
        reader.Expect(",");
    }
}

}  // namespace

bool EditLog::Apply(Document& document, Operation operation, EditError& error,
                    std::string& message) {
    Operation inverse = RemoveClipOperation{};
    if (!ApplyOperation(document, operation, inverse, error, message))
        return false;
    applied_.push_back({std::move(operation), std::move(inverse)});
    undone_.clear();
    return true;
}

bool EditLog::Undo(Document& document, EditError& error, std::string& message) {
    if (applied_.empty()) {
        error = EditError::EmptyUndo;
        message = "undo stack is empty";
        return false;
    }
    Operation inverse = applied_.back().inverse;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(document, inverse, ignored, error, message))
        return false;
    undone_.push_back(std::move(applied_.back()));
    applied_.pop_back();
    return true;
}

bool EditLog::Redo(Document& document, EditError& error, std::string& message) {
    if (undone_.empty()) {
        error = EditError::EmptyRedo;
        message = "redo stack is empty";
        return false;
    }
    Operation operation = undone_.back().op;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(document, operation, ignored, error, message))
        return false;
    applied_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

size_t EditLog::AppliedCount() const { return applied_.size(); }
size_t EditLog::UndoneCount() const { return undone_.size(); }
const std::vector<Entry>& EditLog::AppliedEntries() const { return applied_; }
const std::vector<Entry>& EditLog::UndoneEntries() const { return undone_; }

bool EditLog::Save(const std::string& path, EditError& error,
                   std::string& message) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = EditError::IoError;
        message = "unable to create edit log '" + path + "'";
        return false;
    }
    output << Serialize();
    if (!output) {
        error = EditError::IoError;
        message = "unable to write edit log '" + path + "'";
        return false;
    }
    error = EditError::None;
    message.clear();
    return true;
}

bool EditLog::Load(const std::string& path, EditLog& output, EditError& error,
                   std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = EditError::IoError;
        message = "unable to open edit log '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = EditError::IoError;
        message = "unable to read edit log '" + path + "'";
        return false;
    }
    return Deserialize(contents.str(), output, error, message);
}

std::string EditLog::Serialize() const {
    std::ostringstream output;
    output << "{\"version\":1,\"applied\":";
    WriteEntries(output, applied_);
    output << ",\"undone\":";
    WriteEntries(output, undone_);
    output << "}\n";
    return output.str();
}

bool EditLog::Deserialize(const std::string& json, EditLog& output,
                          EditError& error, std::string& message) {
    try {
        LogReader reader(json);
        reader.Expect("{\"version\":1,\"applied\":");
        EditLog parsed;
        parsed.applied_ = ReadEntries(reader);
        reader.Expect(",\"undone\":");
        parsed.undone_ = ReadEntries(reader);
        reader.Expect("}");
        reader.Finish();
        output = std::move(parsed);
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}

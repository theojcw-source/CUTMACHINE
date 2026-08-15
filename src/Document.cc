#include "Project.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    int64_t number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (position_ != input_.size()) {
            Fail("unexpected trailing content");
        }
        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& message) const {
        throw std::runtime_error("JSON byte " + std::to_string(position_) +
                                 ": " + message);
    }

    void SkipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    char Take() {
        if (position_ >= input_.size()) {
            Fail("unexpected end of input");
        }
        return input_[position_++];
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue ParseValue() {
        if (position_ >= input_.size()) {
            Fail("expected a value");
        }
        switch (input_[position_]) {
            case '{':
                return ParseObject();
            case '[':
                return ParseArray();
            case '"': {
                JsonValue value;
                value.type = JsonValue::Type::String;
                value.string = ParseString();
                return value;
            }
            case 't':
                return ParseLiteral("true", true);
            case 'f':
                return ParseLiteral("false", false);
            case 'n':
                return ParseNull();
            default:
                if (input_[position_] == '-' ||
                    std::isdigit(
                        static_cast<unsigned char>(input_[position_]))) {
                    return ParseNumber();
                }
                Fail("expected an object, array, string, integer or literal");
        }
    }

    JsonValue ParseObject() {
        Take();
        JsonValue value;
        value.type = JsonValue::Type::Object;
        SkipWhitespace();
        if (Consume('}')) {
            return value;
        }
        while (true) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                Fail("expected an object key");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) {
                Fail("expected ':' after object key");
            }
            SkipWhitespace();
            if (!value.object.emplace(key, ParseValue()).second) {
                Fail("duplicate object key '" + key + "'");
            }
            SkipWhitespace();
            if (Consume('}')) {
                return value;
            }
            if (!Consume(',')) {
                Fail("expected ',' or '}'");
            }
            SkipWhitespace();
        }
    }

    JsonValue ParseArray() {
        Take();
        JsonValue value;
        value.type = JsonValue::Type::Array;
        SkipWhitespace();
        if (Consume(']')) {
            return value;
        }
        while (true) {
            value.array.push_back(ParseValue());
            SkipWhitespace();
            if (Consume(']')) {
                return value;
            }
            if (!Consume(',')) {
                Fail("expected ',' or ']'");
            }
            SkipWhitespace();
        }
    }

    static void AppendUtf8(uint32_t codePoint, std::string& output) {
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    std::string ParseString() {
        Take();
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(Take());
            if (character == '"') {
                return result;
            }
            if (character < 0x20) {
                Fail("unescaped control character in string");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            const char escape = Take();
            switch (escape) {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    uint32_t codePoint = 0;
                    for (int index = 0; index < 4; ++index) {
                        const char hex = Take();
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9')
                            codePoint |= hex - '0';
                        else if (hex >= 'a' && hex <= 'f')
                            codePoint |= hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F')
                            codePoint |= hex - 'A' + 10;
                        else
                            Fail("invalid Unicode escape");
                    }
                    if (codePoint >= 0xd800 && codePoint <= 0xdfff) {
                        Fail("UTF-16 surrogate escapes are not supported");
                    }
                    AppendUtf8(codePoint, result);
                    break;
                }
                default:
                    Fail("invalid string escape");
            }
        }
        Fail("unterminated string");
    }

    JsonValue ParseNumber() {
        const size_t start = position_;
        Consume('-');
        if (position_ >= input_.size()) {
            Fail("incomplete integer");
        }
        if (Consume('0')) {
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("leading zero in integer");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("invalid integer");
            }
            while (
                position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' ||
             input_[position_] == 'E')) {
            Fail("floating-point JSON numbers are forbidden in this document");
        }
        const std::string text = input_.substr(start, position_ - start);
        errno = 0;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0') {
            Fail("integer is outside int64_t range");
        }
        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.number = static_cast<int64_t>(parsed);
        return value;
    }

    JsonValue ParseLiteral(const char* literal, bool boolean) {
        for (size_t index = 0; literal[index]; ++index) {
            if (Take() != literal[index]) {
                Fail("invalid literal");
            }
        }
        JsonValue value;
        value.type = JsonValue::Type::Boolean;
        value.boolean = boolean;
        return value;
    }

    JsonValue ParseNull() {
        const char literal[] = "null";
        for (size_t index = 0; literal[index]; ++index) {
            if (Take() != literal[index]) {
                Fail("invalid literal");
            }
        }
        return {};
    }

    const std::string& input_;
    size_t position_ = 0;
};

const JsonValue& Require(const JsonValue& object, const std::string& key,
                         JsonValue::Type type, const std::string& context) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error(context + " must be an object");
    }
    const auto found = object.object.find(key);
    if (found == object.object.end()) {
        throw std::runtime_error(context + " is missing '" + key + "'");
    }
    if (found->second.type != type) {
        throw std::runtime_error(context + "." + key +
                                 " has the wrong JSON type");
    }
    return found->second;
}

const JsonValue* Optional(const JsonValue& object, const std::string& key,
                          JsonValue::Type type, const std::string& context) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error(context + " must be an object");
    }
    const auto found = object.object.find(key);
    if (found == object.object.end()) return nullptr;
    if (found->second.type != type) {
        throw std::runtime_error(context + "." + key +
                                 " has the wrong JSON type");
    }
    return &found->second;
}

int32_t Int32(const JsonValue& object, const std::string& key,
              const std::string& context) {
    const int64_t value =
        Require(object, key, JsonValue::Type::Number, context).number;
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(context + "." + key +
                                 " is outside int32_t range");
    }
    return static_cast<int32_t>(value);
}

RationalTime ParseTime(const JsonValue& value, const std::string& context) {
    return {Require(value, "value", JsonValue::Type::Number, context).number,
            Int32(value, "rate", context)};
}

EffectParamValue ParseEffectParamValue(const JsonValue& value,
                                       const std::string& context) {
    return {Int32(value, "num", context), Int32(value, "den", context)};
}

std::string Escape(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

void WriteTime(std::ostringstream& output, const RationalTime& time) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

void WriteEffectParamValue(std::ostringstream& output,
                           const EffectParamValue& value) {
    output << "{\"num\":" << value.num << ",\"den\":" << value.den << "}";
}

bool RegisterId(const Ulid& id, const std::string& context, std::set<Ulid>& ids,
                std::string& error) {
    if (!IsValidUlid(id)) {
        error = context + " has invalid ULID '" + id + "'";
        return false;
    }
    if (!ids.insert(id).second) {
        error = "duplicate ID '" + id + "' at " + context;
        return false;
    }
    return true;
}

Ulid MigratedProjectId(const Ulid& sequenceId) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : sequenceId) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    static constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string suffix(13, '0');
    for (size_t index = suffix.size(); index-- > 0;) {
        suffix[index] = alphabet[hash & 31U];
        hash >>= 5;
    }
    return "0000000000001" + suffix;
}

}  // namespace

Project Project::FromDocument(Document document, std::string projectName) {
    Project project(std::move(projectName));
    project.id = MigratedProjectId(document.sequence.id);
    project.settings.color_management = document.color_management;
    project.rushes = std::move(document.library);
    project.bins = std::move(document.bins);
    project.sources = std::move(document.sources);
    project.timelines = {std::move(document.sequence)};
    project.active_timeline_id = project.timelines.front().id;
    return project;
}

bool Project::Load(const std::string& path, Project& output,
                   std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open project '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read project '" + path + "'";
        return false;
    }
    return LoadFromString(contents.str(), output, error);
}

bool Project::LoadFromString(const std::string& json, Project& output,
                             std::string& error) {
    try {
        const JsonValue root = JsonParser(json).Parse();
        const JsonValue* format = Optional(root, "project_format",
                                           JsonValue::Type::String, "project");
        if (!format) throw std::runtime_error("missing project format");
        if (format->string != "cutmachine-project")
            throw std::runtime_error("unsupported project format '" +
                                     format->string + "'");
        if (Int32(root, "project_version", "project") != 2)
            throw std::runtime_error("unsupported project version");

        Project parsed(
            Require(root, "name", JsonValue::Type::String, "project").string);
        parsed.id =
            Require(root, "id", JsonValue::Type::String, "project").string;
        parsed.active_timeline_id = Require(root, "active_timeline_id",
                                            JsonValue::Type::String, "project")
                                        .string;
        parsed.timelines.clear();

        const JsonValue& documents = Require(root, "timeline_snapshots",
                                             JsonValue::Type::Array, "project");
        if (documents.array.empty())
            throw std::runtime_error("project has no timeline documents");
        std::optional<Document> shared;
        for (size_t index = 0; index < documents.array.size(); ++index) {
            const JsonValue& value = documents.array[index];
            if (value.type != JsonValue::Type::String)
                throw std::runtime_error("project.timeline_snapshots[" +
                                         std::to_string(index) +
                                         "] has the wrong JSON type");
            Document document;
            std::string documentError;
            if (!Document::LoadFromString(value.string, document,
                                          documentError))
                throw std::runtime_error("invalid timeline document " +
                                         std::to_string(index) + ": " +
                                         documentError);
            if (!shared) {
                shared = document;
                parsed.settings.color_management = document.color_management;
                parsed.rushes = document.library;
                parsed.bins = document.bins;
                parsed.sources = document.sources;
            } else {
                Document candidate = document;
                candidate.sequence = shared->sequence;
                if (candidate.SaveToString() != shared->SaveToString())
                    throw std::runtime_error(
                        "timeline documents disagree on shared project data");
            }
            parsed.timelines.push_back(std::move(document.sequence));
        }

        parsed.bin_metadata.clear();
        if (const JsonValue* metadata = Optional(
                root, "bin_metadata", JsonValue::Type::Array, "project")) {
            for (size_t index = 0; index < metadata->array.size(); ++index) {
                const JsonValue& item = metadata->array[index];
                const std::string context =
                    "project.bin_metadata[" + std::to_string(index) + "]";
                ProjectBinMetadata entry;
                const Ulid itemId =
                    Require(item, "item_id", JsonValue::Type::String, context)
                        .string;
                entry.description = Require(item, "description",
                                            JsonValue::Type::String, context)
                                        .string;
                const int32_t rating = Int32(item, "rating", context);
                if (rating < 0)
                    throw std::runtime_error(context +
                                             ".rating cannot be negative");
                entry.rating = static_cast<uint32_t>(rating);
                const int64_t order = Require(item, "insert_order",
                                              JsonValue::Type::Number, context)
                                          .number;
                if (order < 0)
                    throw std::runtime_error(
                        context + ".insert_order cannot be negative");
                entry.insert_order = static_cast<uint64_t>(order);
                const JsonValue& tags =
                    Require(item, "tags", JsonValue::Type::Array, context);
                for (size_t tagIndex = 0; tagIndex < tags.array.size();
                     ++tagIndex) {
                    if (tags.array[tagIndex].type != JsonValue::Type::String)
                        throw std::runtime_error(context + ".tags[" +
                                                 std::to_string(tagIndex) +
                                                 "] must be a string");
                    entry.tags.push_back(tags.array[tagIndex].string);
                }
                if (!parsed.bin_metadata.emplace(itemId, std::move(entry))
                         .second)
                    throw std::runtime_error("duplicate bin metadata for '" +
                                             itemId + "'");
            }
        }

        parsed.timeline_bin_ids.clear();
        if (const JsonValue* placements = Optional(
                root, "timeline_bin_ids", JsonValue::Type::Array, "project")) {
            for (size_t index = 0; index < placements->array.size(); ++index) {
                const JsonValue& item = placements->array[index];
                const std::string context =
                    "project.timeline_bin_ids[" + std::to_string(index) + "]";
                const Ulid timelineId =
                    Require(item, "timeline_id", JsonValue::Type::String,
                            context)
                        .string;
                const Ulid binId =
                    Require(item, "bin_id", JsonValue::Type::String, context)
                        .string;
                if (!parsed.timeline_bin_ids.emplace(timelineId, binId).second)
                    throw std::runtime_error(
                        "duplicate timeline bin placement for '" + timelineId +
                        "'");
            }
        }

        if (!parsed.Validate(error)) return false;
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool Project::Save(const std::string& path, std::string& error) const {
    if (!Validate(error)) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create project '" + path + "'";
        return false;
    }
    output << SaveToString();
    if (!output) {
        error = "unable to write project '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

std::string Project::SaveToString() const {
    std::ostringstream output;
    output << "{\n  \"project_format\":\"cutmachine-project\","
           << "\n  \"project_version\":2," << "\n  \"id\":\"" << Escape(id)
           << "\"," << "\n  \"name\":\"" << Escape(name) << "\","
           << "\n  \"active_timeline_id\":\"" << Escape(active_timeline_id)
           << "\"," << "\n  \"timeline_snapshots\":[";
    for (size_t index = 0; index < timelines.size(); ++index) {
        Document document;
        document.sequence = timelines[index];
        document.color_management = settings.color_management;
        document.library = rushes;
        document.bins = bins;
        document.sources = sources;
        output << (index == 0 ? "\n" : ",\n") << "    \""
               << Escape(document.SaveToString()) << "\"";
    }
    if (!timelines.empty()) output << '\n';
    output << "  ],\n  \"bin_metadata\":[";
    size_t metadataIndex = 0;
    for (const auto& item : bin_metadata) {
        output << (metadataIndex++ == 0 ? "\n" : ",\n") << "    {\"item_id\":\""
               << Escape(item.first) << "\",\"description\":\""
               << Escape(item.second.description)
               << "\",\"rating\":" << item.second.rating << ",\"tags\":[";
        for (size_t tagIndex = 0; tagIndex < item.second.tags.size();
             ++tagIndex) {
            if (tagIndex) output << ',';
            output << "\"" << Escape(item.second.tags[tagIndex]) << "\"";
        }
        output << "],\"insert_order\":" << item.second.insert_order << '}';
    }
    if (!bin_metadata.empty()) output << '\n';
    output << "  ],\n  \"timeline_bin_ids\":[";
    size_t placementIndex = 0;
    for (const auto& item : timeline_bin_ids) {
        output << (placementIndex++ == 0 ? "\n" : ",\n")
               << "    {\"timeline_id\":\"" << Escape(item.first)
               << "\",\"bin_id\":\"" << Escape(item.second) << "\"}";
    }
    if (!timeline_bin_ids.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

bool Document::Load(const std::string& path, Document& output,
                    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open document '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read document '" + path + "'";
        return false;
    }
    return LoadFromString(contents.str(), output, error);
}

bool Document::LoadFromString(const std::string& json, Document& output,
                              std::string& error) {
    return LoadFromString(json, output, error, true);
}

bool Document::LoadFromString(const std::string& json, Document& output,
                              std::string& error, bool validate) {
    try {
        const JsonValue root = JsonParser(json).Parse();
        Document parsed;
        parsed.version = Int32(root, "version", "document");
        if (parsed.version != 4) {
            throw std::runtime_error("unsupported document version " +
                                     std::to_string(parsed.version));
        }

        if (const JsonValue* color =
                Optional(root, "color_management", JsonValue::Type::Object,
                         "document")) {
            const std::string context = "document.color_management";
            parsed.color_management.enabled =
                Require(*color, "enabled", JsonValue::Type::Boolean, context)
                    .boolean;
            parsed.color_management.input_gamut =
                Require(*color, "input_gamut", JsonValue::Type::String, context)
                    .string;
            parsed.color_management.input_transfer =
                Require(*color, "input_transfer", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.input_ycbcr_matrix =
                Require(*color, "input_ycbcr_matrix", JsonValue::Type::String,
                        context)
                    .string;
            if (const JsonValue* range = Optional(
                    *color, "input_range", JsonValue::Type::String, context))
                parsed.color_management.input_range = range->string;
            parsed.color_management.working_gamut =
                Require(*color, "working_gamut", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.output_gamut =
                Require(*color, "output_gamut", JsonValue::Type::String,
                        context)
                    .string;
            parsed.color_management.output_transfer =
                Require(*color, "output_transfer", JsonValue::Type::String,
                        context)
                    .string;
        }

        const JsonValue& sequence =
            Require(root, "sequence", JsonValue::Type::Object, "document");
        const JsonValue* sequenceValue = &sequence;
        {
            const std::string context = "document.sequence";
            parsed.sequence.id =
                Require(sequence, "id", JsonValue::Type::String, context)
                    .string;
            parsed.sequence.name =
                Require(sequence, "name", JsonValue::Type::String, context)
                    .string;
            parsed.sequence.width = Int32(sequence, "width", context);
            parsed.sequence.height = Int32(sequence, "height", context);
            const JsonValue& rate = Require(sequence, "frame_rate",
                                            JsonValue::Type::Object, context);
            parsed.sequence.frame_rate = {
                Int32(rate, "num", context + ".frame_rate"),
                Int32(rate, "den", context + ".frame_rate")};
        }

        {
            const JsonValue& library =
                Require(root, "library", JsonValue::Type::Array, "document");
            for (size_t index = 0; index < library.array.size(); ++index) {
                const JsonValue& item = library.array[index];
                const std::string context =
                    "library[" + std::to_string(index) + "]";
                LibraryMedia media;
                media.id = Require(item, "id", JsonValue::Type::String, context)
                               .string;
                media.path =
                    Require(item, "path", JsonValue::Type::String, context)
                        .string;
                media.filename =
                    Require(item, "filename", JsonValue::Type::String, context)
                        .string;
                if (const JsonValue* bin = Optional(
                        item, "bin_id", JsonValue::Type::String, context))
                    media.bin_id = bin->string;
                if (const JsonValue* proxy = Optional(
                        item, "proxy_path", JsonValue::Type::String, context))
                    media.proxy_path = proxy->string;
                const JsonValue* codec =
                    Optional(item, "codec", JsonValue::Type::String, context);
                if (!codec) {
                    media.metadata_complete = false;
                } else {
                    media.codec = codec->string;
                    media.width = Int32(item, "width", context);
                    media.height = Int32(item, "height", context);
                    if (const JsonValue* value =
                            Optional(item, "pixel_format",
                                     JsonValue::Type::String, context))
                        media.pixel_format = value->string;
                    if (const JsonValue* value =
                            Optional(item, "color_range",
                                     JsonValue::Type::String, context))
                        media.color_range = value->string;
                    if (const JsonValue* value =
                            Optional(item, "color_space",
                                     JsonValue::Type::String, context))
                        media.color_space = value->string;
                    if (const JsonValue* value =
                            Optional(item, "color_transfer",
                                     JsonValue::Type::String, context))
                        media.color_transfer = value->string;
                    if (const JsonValue* value =
                            Optional(item, "color_primaries",
                                     JsonValue::Type::String, context))
                        media.color_primaries = value->string;
                    if (Optional(item, "rotation_degrees",
                                 JsonValue::Type::Number, context))
                        media.rotation_degrees =
                            Int32(item, "rotation_degrees", context);
                    media.orientation =
                        Require(item, "orientation", JsonValue::Type::String,
                                context)
                            .string;
                    media.has_audio = Require(item, "has_audio",
                                              JsonValue::Type::Boolean, context)
                                          .boolean;
                    if (media.has_audio) {
                        media.audio_rate = Int32(item, "audio_rate", context);
                        media.audio_channels =
                            Int32(item, "audio_channels", context);
                    }
                }
                const JsonValue& rate =
                    Require(item, "rate", JsonValue::Type::Object, context);
                media.rate = {Int32(rate, "num", context + ".rate"),
                              Int32(rate, "den", context + ".rate")};
                media.duration = ParseTime(
                    Require(item, "duration", JsonValue::Type::Object, context),
                    context + ".duration");
                parsed.library.push_back(std::move(media));
            }
            if (const JsonValue* bins = Optional(
                    root, "bins", JsonValue::Type::Array, "document")) {
                for (size_t index = 0; index < bins->array.size(); ++index) {
                    const JsonValue& item = bins->array[index];
                    const std::string context =
                        "bins[" + std::to_string(index) + "]";
                    DocumentBin bin;
                    bin.id =
                        Require(item, "id", JsonValue::Type::String, context)
                            .string;
                    bin.name =
                        Require(item, "name", JsonValue::Type::String, context)
                            .string;
                    if (const JsonValue* parent =
                            Optional(item, "parent_id", JsonValue::Type::String,
                                     context))
                        bin.parent_id = parent->string;
                    parsed.bins.push_back(std::move(bin));
                }
            }
            const JsonValue& timelineOwner = *sequenceValue;
            const std::string markerRoot = "document.sequence";
            if (const JsonValue* markers =
                    Optional(timelineOwner, "markers", JsonValue::Type::Array,
                             markerRoot)) {
                for (size_t index = 0; index < markers->array.size(); ++index) {
                    const JsonValue& item = markers->array[index];
                    const std::string context =
                        markerRoot + ".markers[" + std::to_string(index) + "]";
                    DocumentMarker marker;
                    marker.id =
                        Require(item, "id", JsonValue::Type::String, context)
                            .string;
                    marker.name =
                        Require(item, "name", JsonValue::Type::String, context)
                            .string;
                    marker.time = ParseTime(
                        Require(item, "time", JsonValue::Type::Object, context),
                        context + ".time");
                    marker.color =
                        Require(item, "color", JsonValue::Type::String, context)
                            .string;
                    marker.category = Require(item, "category",
                                              JsonValue::Type::String, context)
                                          .string;
                    parsed.sequence.markers.push_back(std::move(marker));
                }
            }
            if (const JsonValue* transitions =
                    Optional(timelineOwner, "transitions",
                             JsonValue::Type::Array, markerRoot)) {
                for (size_t index = 0; index < transitions->array.size();
                     ++index) {
                    const JsonValue& item = transitions->array[index];
                    const std::string context = markerRoot + ".transitions[" +
                                                std::to_string(index) + "]";
                    DocumentTransition transition;
                    transition.id =
                        Require(item, "id", JsonValue::Type::String, context)
                            .string;
                    transition.track_id =
                        Require(item, "track_id", JsonValue::Type::String,
                                context)
                            .string;
                    transition.left_clip_id =
                        Require(item, "left_clip_id", JsonValue::Type::String,
                                context)
                            .string;
                    transition.right_clip_id =
                        Require(item, "right_clip_id", JsonValue::Type::String,
                                context)
                            .string;
                    transition.type =
                        Require(item, "type", JsonValue::Type::String, context)
                            .string;
                    transition.duration =
                        ParseTime(Require(item, "duration",
                                          JsonValue::Type::Object, context),
                                  context + ".duration");
                    const std::string alignment =
                        Require(item, "alignment", JsonValue::Type::String,
                                context)
                            .string;
                    if (alignment == "center")
                        transition.alignment = TransitionAlignment::Center;
                    else if (alignment == "start_at_cut")
                        transition.alignment = TransitionAlignment::StartAtCut;
                    else if (alignment == "end_at_cut")
                        transition.alignment = TransitionAlignment::EndAtCut;
                    else
                        throw std::runtime_error(context +
                                                 " has invalid alignment");
                    parsed.sequence.transitions.push_back(
                        std::move(transition));
                }
            }
            if (const JsonValue* styles = Optional(timelineOwner,
                                                    "caption_styles",
                                                    JsonValue::Type::Array,
                                                    markerRoot)) {
                for (size_t index = 0; index < styles->array.size(); ++index) {
                    const JsonValue& item = styles->array[index];
                    const std::string context = markerRoot +
                                                ".caption_styles[" +
                                                std::to_string(index) + "]";
                    CaptionStyle style;
                    style.id =
                        Require(item, "id", JsonValue::Type::String, context)
                            .string;
                    style.font_family = Require(item, "font_family",
                                                JsonValue::Type::String,
                                                context)
                                            .string;
                    style.font_size = Int32(item, "font_size", context);
                    style.color =
                        Require(item, "color", JsonValue::Type::String,
                                context)
                            .string;
                    style.position = Require(item, "position",
                                             JsonValue::Type::String, context)
                                         .string;
                    parsed.sequence.caption_styles.push_back(
                        std::move(style));
                }
            }
            if (const JsonValue* groups = Optional(timelineOwner,
                                                    "multicam_groups",
                                                    JsonValue::Type::Array,
                                                    markerRoot)) {
                for (size_t index = 0; index < groups->array.size();
                     ++index) {
                    const JsonValue& item = groups->array[index];
                    const std::string context = markerRoot +
                                                ".multicam_groups[" +
                                                std::to_string(index) + "]";
                    MulticamGroup group;
                    group.id =
                        Require(item, "id", JsonValue::Type::String, context)
                            .string;
                    group.name =
                        Require(item, "name", JsonValue::Type::String,
                                context)
                            .string;
                    if (const JsonValue* active =
                            Optional(item, "active_angle_id",
                                     JsonValue::Type::String, context))
                        group.active_angle_id = active->string;
                    const JsonValue& angles = Require(
                        item, "angles", JsonValue::Type::Array, context);
                    for (size_t angleIndex = 0;
                         angleIndex < angles.array.size(); ++angleIndex) {
                        const JsonValue& angleValue = angles.array[angleIndex];
                        const std::string angleContext =
                            context + ".angles[" +
                            std::to_string(angleIndex) + "]";
                        MulticamAngle angle;
                        angle.id = Require(angleValue, "id",
                                           JsonValue::Type::String,
                                           angleContext)
                                       .string;
                        angle.name = Require(angleValue, "name",
                                             JsonValue::Type::String,
                                             angleContext)
                                         .string;
                        angle.clip_id = Require(angleValue, "clip_id",
                                                JsonValue::Type::String,
                                                angleContext)
                                            .string;
                        group.angles.push_back(std::move(angle));
                    }
                    parsed.sequence.multicam_groups.push_back(
                        std::move(group));
                }
            }
        }

        const JsonValue& sources =
            Require(root, "sources", JsonValue::Type::Array, "document");
        for (size_t index = 0; index < sources.array.size(); ++index) {
            const JsonValue& item = sources.array[index];
            const std::string context =
                "sources[" + std::to_string(index) + "]";
            DocumentSource source;
            source.id =
                Require(item, "id", JsonValue::Type::String, context).string;
            source.path =
                Require(item, "path", JsonValue::Type::String, context).string;
            const JsonValue& rate =
                Require(item, "rate", JsonValue::Type::Object, context);
            source.rate = {Int32(rate, "num", context + ".rate"),
                           Int32(rate, "den", context + ".rate")};
            source.duration = ParseTime(
                Require(item, "duration", JsonValue::Type::Object, context),
                context + ".duration");
            parsed.sources.push_back(std::move(source));
        }

        const JsonValue& timelineOwner = *sequenceValue;
        const std::string tracksRoot = "document.sequence";
        const JsonValue& tracks = Require(timelineOwner, "tracks",
                                          JsonValue::Type::Array, tracksRoot);
        for (size_t trackIndex = 0; trackIndex < tracks.array.size();
             ++trackIndex) {
            const JsonValue& item = tracks.array[trackIndex];
            const std::string context =
                tracksRoot + ".tracks[" + std::to_string(trackIndex) + "]";
            DocumentTrack track;
            track.id =
                Require(item, "id", JsonValue::Type::String, context).string;
            track.kind =
                Require(item, "kind", JsonValue::Type::String, context).string;
            track.index = Int32(item, "index", context);
            if (const JsonValue* locked =
                    Optional(item, "locked", JsonValue::Type::Boolean, context))
                track.locked = locked->boolean;
            if (const JsonValue* syncLock = Optional(
                    item, "sync_lock", JsonValue::Type::Boolean, context))
                track.sync_lock = syncLock->boolean;
            const JsonValue& clips =
                Require(item, "clips", JsonValue::Type::Array, context);
            for (size_t clipIndex = 0; clipIndex < clips.array.size();
                 ++clipIndex) {
                const JsonValue& clipValue = clips.array[clipIndex];
                const std::string clipContext =
                    context + ".clips[" + std::to_string(clipIndex) + "]";
                DocumentClip clip;
                clip.id = Require(clipValue, "id", JsonValue::Type::String,
                                  clipContext)
                              .string;
                clip.source_id = Require(clipValue, "source_id",
                                         JsonValue::Type::String, clipContext)
                                     .string;
                clip.source_in =
                    ParseTime(Require(clipValue, "source_in",
                                      JsonValue::Type::Object, clipContext),
                              clipContext + ".source_in");
                clip.duration =
                    ParseTime(Require(clipValue, "duration",
                                      JsonValue::Type::Object, clipContext),
                              clipContext + ".duration");
                clip.timeline_in =
                    ParseTime(Require(clipValue, "timeline_in",
                                      JsonValue::Type::Object, clipContext),
                              clipContext + ".timeline_in");
                if (const JsonValue* includeAudio =
                        Optional(clipValue, "include_audio",
                                 JsonValue::Type::Boolean, clipContext))
                    clip.include_audio = includeAudio->boolean;
                if (const JsonValue* linkGroup =
                        Optional(clipValue, "link_group_id",
                                 JsonValue::Type::String, clipContext))
                    clip.link_group_id = linkGroup->string;
                if (const JsonValue* anchor =
                        Optional(clipValue, "sync_anchor_clip_id",
                                 JsonValue::Type::String, clipContext)) {
                    clip.sync_anchor_clip_id = anchor->string;
                    clip.sync_reference_delta =
                        ParseTime(Require(clipValue, "sync_reference_delta",
                                          JsonValue::Type::Object, clipContext),
                                  clipContext + ".sync_reference_delta");
                }
                if (const JsonValue* effects = Optional(
                        clipValue, "effects", JsonValue::Type::Array,
                        clipContext)) {
                    for (size_t effectIndex = 0;
                         effectIndex < effects->array.size(); ++effectIndex) {
                        const JsonValue& effectValue =
                            effects->array[effectIndex];
                        const std::string effectContext =
                            clipContext + ".effects[" +
                            std::to_string(effectIndex) + "]";
                        ClipEffect effect;
                        effect.id = Require(effectValue, "id",
                                            JsonValue::Type::String,
                                            effectContext)
                                        .string;
                        effect.type = Require(effectValue, "type",
                                              JsonValue::Type::String,
                                              effectContext)
                                          .string;
                        const JsonValue& params =
                            Require(effectValue, "params",
                                    JsonValue::Type::Object, effectContext);
                        for (const auto& entry : params.object) {
                            if (entry.second.type != JsonValue::Type::Object) {
                                throw std::runtime_error(
                                    effectContext + ".params['" + entry.first +
                                    "'] has the wrong JSON type");
                            }
                            effect.params.emplace(
                                entry.first,
                                ParseEffectParamValue(
                                    entry.second,
                                    effectContext + ".params['" + entry.first +
                                        "']"));
                        }
                        clip.effects.push_back(std::move(effect));
                    }
                }
                if (const JsonValue* captionGroup =
                        Optional(clipValue, "caption_group_id",
                                 JsonValue::Type::String, clipContext)) {
                    clip.caption_group_id = captionGroup->string;
                    clip.caption_text = Require(clipValue, "caption_text",
                                                JsonValue::Type::String,
                                                clipContext)
                                             .string;
                }
                track.clips.push_back(std::move(clip));
            }
            parsed.sequence.tracks.push_back(std::move(track));
        }

        if (validate && !parsed.Validate(error)) {
            return false;
        }
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool Document::Save(const std::string& path, std::string& error) const {
    if (!Validate(error)) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create document '" + path + "'";
        return false;
    }
    output << SaveToString();
    if (!output) {
        error = "unable to write document '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

std::string Document::SaveToString() const {
    std::ostringstream output;
    output << "{\n  \"version\": " << version << ",\n  \"sequence\":{"
           << "\"id\":\"" << Escape(sequence.id) << "\",\"name\":\""
           << Escape(sequence.name) << "\",\"width\":" << sequence.width
           << ",\"height\":" << sequence.height
           << ",\"frame_rate\":{\"num\":" << sequence.frame_rate.num
           << ",\"den\":" << sequence.frame_rate.den << "},\"markers\":[";
    for (size_t index = 0; index < sequence.markers.size(); ++index) {
        const DocumentMarker& marker = sequence.markers[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(marker.id) << "\",\"name\":\"" << Escape(marker.name)
               << "\",\"time\":";
        WriteTime(output, marker.time);
        output << ",\"color\":\"" << Escape(marker.color)
               << "\",\"category\":\"" << Escape(marker.category) << "\"}";
    }
    if (!sequence.markers.empty()) output << '\n';
    output << "  ],\"transitions\":[";
    for (size_t index = 0; index < sequence.transitions.size(); ++index) {
        const DocumentTransition& transition = sequence.transitions[index];
        const char* alignment =
            transition.alignment == TransitionAlignment::Center ? "center"
            : transition.alignment == TransitionAlignment::StartAtCut
                ? "start_at_cut"
                : "end_at_cut";
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(transition.id) << "\",\"track_id\":\""
               << Escape(transition.track_id) << "\",\"left_clip_id\":\""
               << Escape(transition.left_clip_id) << "\",\"right_clip_id\":\""
               << Escape(transition.right_clip_id) << "\",\"type\":\""
               << Escape(transition.type) << "\",\"duration\":";
        WriteTime(output, transition.duration);
        output << ",\"alignment\":\"" << alignment << "\"}";
    }
    if (!sequence.transitions.empty()) output << '\n';
    output << "  ],\"tracks\":[";
    for (size_t trackIndex = 0; trackIndex < sequence.tracks.size();
         ++trackIndex) {
        const DocumentTrack& track = sequence.tracks[trackIndex];
        output << (trackIndex == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(track.id) << "\",\"kind\":\"" << Escape(track.kind)
               << "\",\"index\":" << track.index
               << ",\"locked\":" << (track.locked ? "true" : "false")
               << ",\"sync_lock\":" << (track.sync_lock ? "true" : "false")
               << ",\"clips\":[";
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            output << (clipIndex == 0 ? "\n" : ",\n") << "      {\"id\":\""
                   << Escape(clip.id) << "\",\"source_id\":\""
                   << Escape(clip.source_id) << "\",\"source_in\":";
            WriteTime(output, clip.source_in);
            output << ",\"duration\":";
            WriteTime(output, clip.duration);
            output << ",\"timeline_in\":";
            WriteTime(output, clip.timeline_in);
            output << ",\"include_audio\":"
                   << (clip.include_audio ? "true" : "false");
            if (!clip.link_group_id.empty())
                output << ",\"link_group_id\":\"" << Escape(clip.link_group_id)
                       << "\"";
            if (!clip.sync_anchor_clip_id.empty()) {
                output << ",\"sync_anchor_clip_id\":\""
                       << Escape(clip.sync_anchor_clip_id)
                       << "\",\"sync_reference_delta\":";
                WriteTime(output, clip.sync_reference_delta);
            }
            output << ",\"effects\":[";
            for (size_t effectIndex = 0; effectIndex < clip.effects.size();
                 ++effectIndex) {
                const ClipEffect& effect = clip.effects[effectIndex];
                output << (effectIndex == 0 ? "\n" : ",\n") << "        {\"id\":\""
                       << Escape(effect.id) << "\",\"type\":\""
                       << Escape(effect.type) << "\",\"params\":{";
                size_t paramIndex = 0;
                for (const auto& param : effect.params) {
                    output << (paramIndex++ == 0 ? "\n" : ",\n")
                           << "          \"" << Escape(param.first) << "\":";
                    WriteEffectParamValue(output, param.second);
                }
                if (!effect.params.empty()) output << '\n' << "        ";
                output << "}}";
            }
            if (!clip.effects.empty()) output << '\n' << "      ";
            output << "]";
            if (!clip.caption_group_id.empty())
                output << ",\"caption_group_id\":\""
                       << Escape(clip.caption_group_id)
                       << "\",\"caption_text\":\""
                       << Escape(clip.caption_text) << "\"";
            output << "}";
        }
        if (!track.clips.empty()) output << '\n';
        output << "    ]}";
    }
    if (!sequence.tracks.empty()) output << '\n';
    output << "  ],\"caption_styles\":[";
    for (size_t index = 0; index < sequence.caption_styles.size(); ++index) {
        const CaptionStyle& style = sequence.caption_styles[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(style.id) << "\",\"font_family\":\""
               << Escape(style.font_family)
               << "\",\"font_size\":" << style.font_size << ",\"color\":\""
               << Escape(style.color) << "\",\"position\":\""
               << Escape(style.position) << "\"}";
    }
    if (!sequence.caption_styles.empty()) output << '\n';
    output << "  ],\"multicam_groups\":[";
    for (size_t index = 0; index < sequence.multicam_groups.size(); ++index) {
        const MulticamGroup& group = sequence.multicam_groups[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(group.id) << "\",\"name\":\"" << Escape(group.name)
               << "\",\"angles\":[";
        for (size_t angleIndex = 0; angleIndex < group.angles.size();
             ++angleIndex) {
            const MulticamAngle& angle = group.angles[angleIndex];
            output << (angleIndex == 0 ? "\n" : ",\n") << "      {\"id\":\""
                   << Escape(angle.id) << "\",\"name\":\""
                   << Escape(angle.name) << "\",\"clip_id\":\""
                   << Escape(angle.clip_id) << "\"}";
        }
        if (!group.angles.empty()) output << '\n' << "    ";
        output << "]";
        if (!group.active_angle_id.empty())
            output << ",\"active_angle_id\":\""
                   << Escape(group.active_angle_id) << "\"";
        output << "}";
    }
    if (!sequence.multicam_groups.empty()) output << '\n';
    output << "  ]}" << ",\n  \"color_management\":{\"enabled\":"
           << (color_management.enabled ? "true" : "false")
           << ",\"input_gamut\":\"" << Escape(color_management.input_gamut)
           << "\",\"input_transfer\":\""
           << Escape(color_management.input_transfer)
           << "\",\"input_ycbcr_matrix\":\""
           << Escape(color_management.input_ycbcr_matrix)
           << "\",\"input_range\":\"" << Escape(color_management.input_range)
           << "\",\"working_gamut\":\""
           << Escape(color_management.working_gamut) << "\",\"output_gamut\":\""
           << Escape(color_management.output_gamut)
           << "\",\"output_transfer\":\""
           << Escape(color_management.output_transfer)
           << "\"},\n  \"library\": [";
    for (size_t index = 0; index < library.size(); ++index) {
        const LibraryMedia& media = library[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(media.id) << "\",\"path\":\"" << Escape(media.path)
               << "\",\"filename\":\"" << Escape(media.filename) << "\"";
        if (!media.bin_id.empty())
            output << ",\"bin_id\":\"" << Escape(media.bin_id) << "\"";
        if (!media.proxy_path.empty())
            output << ",\"proxy_path\":\"" << Escape(media.proxy_path) << "\"";
        if (media.metadata_complete) {
            output << ",\"codec\":\"" << Escape(media.codec)
                   << "\",\"width\":" << media.width
                   << ",\"height\":" << media.height
                   << ",\"rotation_degrees\":" << media.rotation_degrees
                   << ",\"pixel_format\":\"" << Escape(media.pixel_format)
                   << "\",\"color_range\":\"" << Escape(media.color_range)
                   << "\",\"color_space\":\"" << Escape(media.color_space)
                   << "\",\"color_transfer\":\"" << Escape(media.color_transfer)
                   << "\",\"color_primaries\":\""
                   << Escape(media.color_primaries) << "\"";
        }
        output << ",\"rate\":{\"num\":" << media.rate.num
               << ",\"den\":" << media.rate.den << "},\"duration\":";
        WriteTime(output, media.duration);
        if (media.metadata_complete) {
            output << ",\"orientation\":\"" << Escape(media.orientation)
                   << "\",\"has_audio\":"
                   << (media.has_audio ? "true" : "false");
            if (media.has_audio) {
                output << ",\"audio_rate\":" << media.audio_rate
                       << ",\"audio_channels\":" << media.audio_channels;
            }
        }
        output << "}";
    }
    if (!library.empty()) output << '\n';
    output << "  ],\n  \"bins\": [";
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(bin.id) << "\",\"name\":\"" << Escape(bin.name)
               << "\"";
        if (!bin.parent_id.empty())
            output << ",\"parent_id\":\"" << Escape(bin.parent_id) << "\"";
        output << "}";
    }
    if (!bins.empty()) output << '\n';
    output << "  ],\n  \"sources\": [";
    for (size_t index = 0; index < sources.size(); ++index) {
        const DocumentSource& source = sources[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(source.id) << "\",\"path\":\"" << Escape(source.path)
               << "\",\"rate\":{\"num\":" << source.rate.num
               << ",\"den\":" << source.rate.den << "},\"duration\":";
        WriteTime(output, source.duration);
        output << "}";
    }
    if (!sources.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

bool Document::Validate(std::string& error) const {
    if (version != 4) {
        error = "unsupported document version " + std::to_string(version);
        return false;
    }
    if (!IsValidUlid(sequence.id) || sequence.name.empty() ||
        sequence.width <= 0 || sequence.height <= 0 || sequence.width > 16384 ||
        sequence.height > 16384 || sequence.frame_rate.num <= 0 ||
        sequence.frame_rate.den <= 0) {
        error = "sequence has an invalid ID, name, dimensions or frame rate";
        return false;
    }

    const auto oneOf = [](const std::string& value,
                          std::initializer_list<const char*> allowed) {
        return std::any_of(allowed.begin(), allowed.end(),
                           [&](const char* item) { return value == item; });
    };
    if (!oneOf(color_management.input_gamut,
               {"rec709", "sony_sgamut3_cine", "sony_sgamut3", "rec2020"}) ||
        !oneOf(color_management.input_transfer,
               {"rec709", "sony_slog3", "linear"}) ||
        !oneOf(color_management.input_ycbcr_matrix,
               {"auto", "bt709", "bt2020_ncl"}) ||
        !oneOf(color_management.input_range, {"auto", "full", "limited"}) ||
        !oneOf(color_management.working_gamut,
               {"acescct", "rec2020", "rec709"}) ||
        !oneOf(color_management.output_gamut, {"rec709", "rec2020"}) ||
        !oneOf(color_management.output_transfer, {"rec709", "hlg"})) {
        error =
            "color_management contains an unsupported color space or transfer";
        return false;
    }
    if (color_management.output_transfer == "hlg" &&
        color_management.output_gamut != "rec2020") {
        error = "HLG output requires the rec2020 output gamut";
        return false;
    }

    std::set<Ulid> ids;
    if (!RegisterId(sequence.id, "sequence", ids, error)) return false;
    std::set<Ulid> binIds;
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        const std::string context = "bin " + std::to_string(index);
        if (!RegisterId(bin.id, context, ids, error)) return false;
        if (bin.name.empty()) {
            error = context + " has an empty name";
            return false;
        }
        binIds.insert(bin.id);
    }
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        if (!bin.parent_id.empty() &&
            binIds.find(bin.parent_id) == binIds.end()) {
            error = "bin " + std::to_string(index) +
                    " references unknown parent_id '" + bin.parent_id + "'";
            return false;
        }
        std::set<Ulid> ancestors;
        const DocumentBin* cursor = &bin;
        while (!cursor->parent_id.empty()) {
            if (!ancestors.insert(cursor->id).second) {
                error = "bin hierarchy contains a cycle at '" + bin.id + "'";
                return false;
            }
            cursor = FindBin(cursor->parent_id);
            if (!cursor) break;
        }
    }
    const auto validMarkerText = [](const std::string& value, size_t maximum) {
        if (value.empty() || value.size() > maximum) return false;
        return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        });
    };
    for (size_t index = 0; index < sequence.markers.size(); ++index) {
        const DocumentMarker& marker = sequence.markers[index];
        const std::string context = "marker " + std::to_string(index);
        if (!RegisterId(marker.id, context, ids, error)) return false;
        if (!validMarkerText(marker.name, 4096) ||
            !validMarkerText(marker.color, 64) ||
            !validMarkerText(marker.category, 128)) {
            error = context + " has an invalid name, color or category";
            return false;
        }
        if (marker.time.rate <= 0 || marker.time.value < 0) {
            error = context + " has an invalid time";
            return false;
        }
    }
    std::set<Ulid> libraryIds;
    for (size_t index = 0; index < library.size(); ++index) {
        const LibraryMedia& media = library[index];
        const std::string context = "library media " + std::to_string(index);
        if (!IsValidUlid(media.id)) {
            error = context + " has invalid ULID '" + media.id + "'";
            return false;
        }
        if (!libraryIds.insert(media.id).second) {
            error = "duplicate library ID '" + media.id + "'";
            return false;
        }
        if (media.path.empty() || media.filename.empty()) {
            error = context + " has an empty path or filename";
            return false;
        }
        if (!media.bin_id.empty() &&
            binIds.find(media.bin_id) == binIds.end()) {
            error =
                context + " references unknown bin_id '" + media.bin_id + "'";
            return false;
        }
        if (media.rate.num <= 0 || media.rate.den <= 0 ||
            media.duration.rate <= 0 || media.duration.value <= 0) {
            error = context + " has an invalid rate or duration";
            return false;
        }
        if (media.metadata_complete) {
            if (media.codec.empty() || media.width <= 0 || media.height <= 0 ||
                media.rotation_degrees < -180 || media.rotation_degrees > 180 ||
                (media.orientation != "landscape" &&
                 media.orientation != "portrait" &&
                 media.orientation != "square")) {
                error = context + " has invalid video metadata";
                return false;
            }
            if (media.has_audio &&
                (media.audio_rate <= 0 || media.audio_channels <= 0)) {
                error = context + " has invalid audio metadata";
                return false;
            }
        }
    }
    std::set<Ulid> sourceIds;
    for (size_t index = 0; index < sources.size(); ++index) {
        const DocumentSource& source = sources[index];
        const std::string context = "source " + std::to_string(index);
        if (!RegisterId(source.id, context, ids, error)) return false;
        sourceIds.insert(source.id);
        if (source.path.empty()) {
            error = context + " ('" + source.id + "') has an empty path";
            return false;
        }
        if (source.rate.num <= 0 || source.rate.den <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative media rate";
            return false;
        }
        if (source.duration.rate <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative duration rate";
            return false;
        }
        if (source.duration.value <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative duration";
            return false;
        }
    }
    // A mounted source deliberately shares its media ULID. Library-only IDs,
    // however, must remain globally distinct from tracks and clips.
    for (const Ulid& libraryId : libraryIds) {
        if (sourceIds.find(libraryId) == sourceIds.end() &&
            !ids.insert(libraryId).second) {
            error = "library ID '" + libraryId +
                    "' collides with another document object";
            return false;
        }
    }

    std::set<int32_t> trackIndices;
    for (size_t trackIndex = 0; trackIndex < sequence.tracks.size();
         ++trackIndex) {
        const DocumentTrack& track = sequence.tracks[trackIndex];
        const std::string trackContext = "track " + std::to_string(trackIndex);
        if (!RegisterId(track.id, trackContext, ids, error)) return false;
        if (!trackIndices.insert(track.index).second) {
            error = "duplicate track index " + std::to_string(track.index);
            return false;
        }
        const DocumentClip* previous = nullptr;
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            const std::string context =
                trackContext + " clip " + std::to_string(clipIndex);
            if (!RegisterId(clip.id, context, ids, error)) return false;
            if (!clip.link_group_id.empty() &&
                !IsValidUlid(clip.link_group_id)) {
                error = context + " ('" + clip.id +
                        "') has invalid link_group_id '" + clip.link_group_id +
                        "'";
                return false;
            }
            if (!clip.sync_anchor_clip_id.empty() &&
                (!IsValidUlid(clip.sync_anchor_clip_id) ||
                 clip.sync_reference_delta.rate <= 0)) {
                error = context + " ('" + clip.id +
                        "') has invalid synchronization reference";
                return false;
            }
            if (clip.link_group_id.empty() &&
                !clip.sync_anchor_clip_id.empty()) {
                error = context + " ('" + clip.id +
                        "') has an incomplete link synchronization state";
                return false;
            }
            if (sourceIds.find(clip.source_id) == sourceIds.end()) {
                error = context + " ('" + clip.id +
                        "') references unknown source_id '" + clip.source_id +
                        "'";
                return false;
            }
            if (clip.source_in.rate <= 0 || clip.duration.rate <= 0 ||
                clip.timeline_in.rate <= 0) {
                error = context + " ('" + clip.id +
                        "') has a zero or negative time rate";
                return false;
            }
            if (clip.duration.value <= 0) {
                error = context + " ('" + clip.id +
                        "') has a zero or negative duration";
                return false;
            }
            if (clip.source_in.value < 0) {
                error = context + " ('" + clip.id +
                        "') has source_in before source start";
                return false;
            }
            if (clip.timeline_in.value < 0) {
                error = context + " ('" + clip.id +
                        "') has timeline_in before zero";
                return false;
            }
            const DocumentSource* source = FindSource(clip.source_id);
            try {
                if (clip.source_in.add(clip.duration) > source->duration) {
                    error = context + " ('" + clip.id +
                            "') has source_in + duration outside source bounds";
                    return false;
                }
                if (previous) {
                    if (clip.timeline_in < previous->timeline_in) {
                        error = trackContext +
                                " clips are not sorted by timeline_in at '" +
                                clip.id + "'";
                        return false;
                    }
                    if (clip.timeline_in <
                        previous->timeline_in.add(previous->duration)) {
                        error = trackContext + " clips overlap at '" + clip.id +
                                "'";
                        return false;
                    }
                }
            } catch (const std::exception& exception) {
                error = context + " has invalid rational time arithmetic: " +
                        exception.what();
                return false;
            }
            for (size_t effectIndex = 0; effectIndex < clip.effects.size();
                 ++effectIndex) {
                const ClipEffect& effect = clip.effects[effectIndex];
                const std::string effectContext =
                    context + " effect " + std::to_string(effectIndex);
                if (!RegisterId(effect.id, effectContext, ids, error))
                    return false;
                if (track.kind != "video") {
                    error = effectContext + " ('" + effect.id +
                            "') is on a non-video track";
                    return false;
                }
                if (effect.type.rfind("color.", 0) != 0 ||
                    effect.type.size() <= 6 || effect.type.size() > 128) {
                    error = effectContext + " ('" + effect.id +
                            "') has an unsupported effect type '" +
                            effect.type + "'";
                    return false;
                }
                for (const auto& param : effect.params) {
                    if (param.first.empty() || param.first.size() > 64) {
                        error = effectContext + " ('" + effect.id +
                                "') has an invalid parameter name";
                        return false;
                    }
                    if (param.second.den <= 0) {
                        error = effectContext + " ('" + effect.id +
                                "') parameter '" + param.first +
                                "' has a non-positive denominator";
                        return false;
                    }
                }
            }
            if (!clip.caption_group_id.empty()) {
                if (!IsValidUlid(clip.caption_group_id)) {
                    error = context + " ('" + clip.id +
                            "') has invalid caption_group_id '" +
                            clip.caption_group_id + "'";
                    return false;
                }
                if (!FindCaptionStyle(clip.caption_group_id)) {
                    error = context + " ('" + clip.id +
                            "') references unknown caption_group_id '" +
                            clip.caption_group_id + "'";
                    return false;
                }
            }
            previous = &clip;
        }
    }
    for (size_t index = 0; index < sequence.transitions.size(); ++index) {
        const DocumentTransition& transition = sequence.transitions[index];
        const std::string context = "transition " + std::to_string(index);
        if (!RegisterId(transition.id, context, ids, error)) return false;
        const DocumentTrack* track = FindTrack(transition.track_id);
        if (!track || track->kind != "video") {
            error = context + " must reference a video track";
            return false;
        }
        if (transition.type != "cross_dissolve" ||
            transition.duration.rate <= 0 || transition.duration.value <= 0) {
            error = context + " has an unsupported type or invalid duration";
            return false;
        }
        size_t leftIndex = track->clips.size();
        size_t rightIndex = track->clips.size();
        for (size_t clipIndex = 0; clipIndex < track->clips.size();
             ++clipIndex) {
            if (track->clips[clipIndex].id == transition.left_clip_id)
                leftIndex = clipIndex;
            if (track->clips[clipIndex].id == transition.right_clip_id)
                rightIndex = clipIndex;
        }
        if (leftIndex == track->clips.size() || rightIndex != leftIndex + 1) {
            error = context + " must reference adjacent clips in edit order";
            return false;
        }
        const DocumentClip& left = track->clips[leftIndex];
        const DocumentClip& right = track->clips[rightIndex];
        try {
            const RationalTime cut = left.timeline_in.add(left.duration);
            if (cut != right.timeline_in) {
                error = context + " clips do not share a cut";
                return false;
            }
            const int64_t frames = transition.duration.to_frames(
                sequence.frame_rate.num, sequence.frame_rate.den);
            const RationalTime exactDuration{
                frames * static_cast<int64_t>(sequence.frame_rate.den),
                sequence.frame_rate.num};
            if (frames <= 0 || exactDuration != transition.duration) {
                error =
                    context + " duration must contain whole sequence frames";
                return false;
            }
            int64_t preFrames = 0;
            int64_t postFrames = 0;
            if (transition.alignment == TransitionAlignment::Center) {
                preFrames = frames / 2;
                postFrames = frames - preFrames;
            } else if (transition.alignment ==
                       TransitionAlignment::StartAtCut) {
                postFrames = frames;
            } else {
                preFrames = frames;
            }
            const RationalTime pre{preFrames * sequence.frame_rate.den,
                                   sequence.frame_rate.num};
            const RationalTime post{postFrames * sequence.frame_rate.den,
                                    sequence.frame_rate.num};
            const DocumentSource* leftSource = FindSource(left.source_id);
            const RationalTime leftOut = left.source_in.add(left.duration);
            if (leftOut.add(post) > leftSource->duration ||
                right.source_in < pre) {
                error = context + " exceeds available media handles";
                return false;
            }
        } catch (const std::exception& exception) {
            error = context + " has invalid rational time arithmetic: " +
                    exception.what();
            return false;
        }
    }
    const auto validCaptionStyleText = [](const std::string& value,
                                          size_t maximum) {
        if (value.empty() || value.size() > maximum) return false;
        return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        });
    };
    for (size_t index = 0; index < sequence.caption_styles.size(); ++index) {
        const CaptionStyle& style = sequence.caption_styles[index];
        const std::string context = "caption style " + std::to_string(index);
        if (!RegisterId(style.id, context, ids, error)) return false;
        if (!validCaptionStyleText(style.font_family, 128) ||
            style.font_size <= 0 ||
            !validCaptionStyleText(style.color, 64) ||
            !validCaptionStyleText(style.position, 64)) {
            error = context +
                    " has an invalid font_family, font_size, color or "
                    "position";
            return false;
        }
    }
    for (size_t groupIndex = 0; groupIndex < sequence.multicam_groups.size();
         ++groupIndex) {
        const MulticamGroup& group = sequence.multicam_groups[groupIndex];
        const std::string groupContext =
            "multicam group " + std::to_string(groupIndex);
        if (!RegisterId(group.id, groupContext, ids, error)) return false;
        if (group.name.empty()) {
            error = groupContext + " has an empty name";
            return false;
        }
        std::set<Ulid> angleIds;
        std::set<Ulid> angleClipIds;
        for (size_t angleIndex = 0; angleIndex < group.angles.size();
             ++angleIndex) {
            const MulticamAngle& angle = group.angles[angleIndex];
            const std::string angleContext =
                groupContext + " angle " + std::to_string(angleIndex);
            if (!RegisterId(angle.id, angleContext, ids, error)) return false;
            angleIds.insert(angle.id);
            if (!angleClipIds.insert(angle.clip_id).second) {
                error = angleContext +
                        " ('" + angle.id +
                        "') references a clip_id already used by another "
                        "angle in this group";
                return false;
            }
            const DocumentTrack* angleTrack = FindTrackForClip(angle.clip_id);
            if (!angleTrack || angleTrack->kind != "video") {
                error = angleContext + " ('" + angle.id +
                        "') must reference a clip on a video track";
                return false;
            }
        }
        if (!group.active_angle_id.empty() &&
            angleIds.find(group.active_angle_id) == angleIds.end()) {
            error = groupContext + " active_angle_id '" +
                    group.active_angle_id +
                    "' does not reference one of its own angles";
            return false;
        }
    }
    error.clear();
    return true;
}

const DocumentSource* Document::FindSource(const Ulid& id) const {
    for (const DocumentSource& source : sources) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

DocumentSource* Document::FindSource(const Ulid& id) {
    for (DocumentSource& source : sources) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

const LibraryMedia* Document::FindLibraryMedia(const Ulid& id) const {
    for (const LibraryMedia& media : library) {
        if (media.id == id) return &media;
    }
    return nullptr;
}

LibraryMedia* Document::FindLibraryMedia(const Ulid& id) {
    for (LibraryMedia& media : library) {
        if (media.id == id) return &media;
    }
    return nullptr;
}

const DocumentBin* Document::FindBin(const Ulid& id) const {
    for (const DocumentBin& bin : bins) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

DocumentBin* Document::FindBin(const Ulid& id) {
    for (DocumentBin& bin : bins) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

const DocumentMarker* Document::FindMarker(const Ulid& id) const {
    for (const DocumentMarker& marker : sequence.markers) {
        if (marker.id == id) return &marker;
    }
    return nullptr;
}

DocumentMarker* Document::FindMarker(const Ulid& id) {
    for (DocumentMarker& marker : sequence.markers) {
        if (marker.id == id) return &marker;
    }
    return nullptr;
}

const DocumentTransition* Document::FindTransition(const Ulid& id) const {
    for (const DocumentTransition& transition : sequence.transitions) {
        if (transition.id == id) return &transition;
    }
    return nullptr;
}

DocumentTransition* Document::FindTransition(const Ulid& id) {
    for (DocumentTransition& transition : sequence.transitions) {
        if (transition.id == id) return &transition;
    }
    return nullptr;
}

const CaptionStyle* Document::FindCaptionStyle(const Ulid& id) const {
    for (const CaptionStyle& style : sequence.caption_styles) {
        if (style.id == id) return &style;
    }
    return nullptr;
}

CaptionStyle* Document::FindCaptionStyle(const Ulid& id) {
    for (CaptionStyle& style : sequence.caption_styles) {
        if (style.id == id) return &style;
    }
    return nullptr;
}

const MulticamGroup* Document::FindMulticamGroup(const Ulid& id) const {
    for (const MulticamGroup& group : sequence.multicam_groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

MulticamGroup* Document::FindMulticamGroup(const Ulid& id) {
    for (MulticamGroup& group : sequence.multicam_groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrack(const Ulid& id) const {
    for (const DocumentTrack& track : sequence.tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

DocumentTrack* Document::FindTrack(const Ulid& id) {
    for (DocumentTrack& track : sequence.tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

const DocumentClip* Document::FindClip(const Ulid& id) const {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

DocumentClip* Document::FindClip(const Ulid& id) {
    for (DocumentTrack& track : sequence.tracks) {
        for (DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) const {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) {
    for (DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

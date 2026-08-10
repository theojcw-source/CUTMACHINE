#include "Document.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
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

}  // namespace

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
    try {
        const JsonValue root = JsonParser(json).Parse();
        Document parsed;
        parsed.version = Int32(root, "version", "document");

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

        const JsonValue& tracks =
            Require(root, "tracks", JsonValue::Type::Array, "document");
        for (size_t trackIndex = 0; trackIndex < tracks.array.size();
             ++trackIndex) {
            const JsonValue& item = tracks.array[trackIndex];
            const std::string context =
                "tracks[" + std::to_string(trackIndex) + "]";
            DocumentTrack track;
            track.id =
                Require(item, "id", JsonValue::Type::String, context).string;
            track.kind =
                Require(item, "kind", JsonValue::Type::String, context).string;
            track.index = Int32(item, "index", context);
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
                track.clips.push_back(std::move(clip));
            }
            parsed.tracks.push_back(std::move(track));
        }

        if (!parsed.Validate(error)) {
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
    output << "{\n  \"version\": " << version << ",\n  \"sources\": [";
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
    output << "  ],\n  \"tracks\": [";
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const DocumentTrack& track = tracks[trackIndex];
        output << (trackIndex == 0 ? "\n" : ",\n") << "    {\"id\":\""
               << Escape(track.id) << "\",\"kind\":\"" << Escape(track.kind)
               << "\",\"index\":" << track.index << ",\"clips\":[";
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
            output << "}";
        }
        if (!track.clips.empty()) output << '\n';
        output << "    ]}";
    }
    if (!tracks.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

bool Document::Validate(std::string& error) const {
    if (version != 1) {
        error = "unsupported document version " + std::to_string(version);
        return false;
    }

    std::set<Ulid> ids;
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

    std::set<int32_t> trackIndices;
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const DocumentTrack& track = tracks[trackIndex];
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
            previous = &clip;
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

const DocumentTrack* Document::FindTrack(const Ulid& id) const {
    for (const DocumentTrack& track : tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

DocumentTrack* Document::FindTrack(const Ulid& id) {
    for (DocumentTrack& track : tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

const DocumentClip* Document::FindClip(const Ulid& id) const {
    for (const DocumentTrack& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

DocumentClip* Document::FindClip(const Ulid& id) {
    for (DocumentTrack& track : tracks) {
        for (DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) const {
    for (const DocumentTrack& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) {
    for (DocumentTrack& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

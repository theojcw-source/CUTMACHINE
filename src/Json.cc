#include "Json.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace mcp_json {

namespace {

// Bound well under DBL_MAX_10_EXP (308) so an accepted literal can never
// round to +/-infinity regardless of mantissa magnitude or sign handling
// elsewhere. This is a deliberately conservative "reject before it can go
// wrong" check, not an attempt to reproduce IEEE 754 rounding.
constexpr int kMaxDecimalExponent = 300;

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool ParseDocument(Value& output, std::string& error) {
        SkipWhitespace();
        if (!ParseValue(output, error)) return false;
        SkipWhitespace();
        if (position_ != text_.size()) {
            error = "unexpected trailing content at byte " +
                    std::to_string(position_);
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace() {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character == ' ' || character == '\t' || character == '\n' ||
                character == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool AtEnd() const { return position_ >= text_.size(); }
    char Peek() const { return text_[position_]; }

    bool Fail(const std::string& what, std::string& error) {
        error = what + " at byte " + std::to_string(position_);
        return false;
    }

    bool ParseValue(Value& output, std::string& error) {
        SkipWhitespace();
        if (AtEnd()) return Fail("unexpected end of JSON", error);
        const char lead = Peek();
        if (lead == '{') return ParseObject(output, error);
        if (lead == '[') return ParseArray(output, error);
        if (lead == '"') return ParseString(output, error);
        if (lead == 't' || lead == 'f') return ParseBool(output, error);
        if (lead == 'n') return ParseNull(output, error);
        if (lead == '-' || (lead >= '0' && lead <= '9'))
            return ParseNumber(output, error);
        return Fail("unexpected character", error);
    }

    bool ParseObject(Value& output, std::string& error) {
        output = Value::MakeObject();
        ++position_;  // consume '{'
        SkipWhitespace();
        if (!AtEnd() && Peek() == '}') {
            ++position_;
            return true;
        }
        while (true) {
            SkipWhitespace();
            if (AtEnd() || Peek() != '"')
                return Fail("expected an object key", error);
            Value key;
            if (!ParseString(key, error)) return false;
            SkipWhitespace();
            if (AtEnd() || Peek() != ':')
                return Fail("expected ':' after object key", error);
            ++position_;
            Value value;
            if (!ParseValue(value, error)) return false;
            for (const auto& existing : output.AsObject()) {
                if (existing.first == key.AsString()) {
                    return Fail("duplicate object key '" + key.AsString() + "'",
                                error);
                }
            }
            output.Set(key.AsString(), std::move(value));
            SkipWhitespace();
            if (AtEnd()) return Fail("unterminated object", error);
            if (Peek() == ',') {
                ++position_;
                continue;
            }
            if (Peek() == '}') {
                ++position_;
                return true;
            }
            return Fail("expected ',' or '}' in object", error);
        }
    }

    bool ParseArray(Value& output, std::string& error) {
        output = Value::MakeArray();
        ++position_;  // consume '['
        SkipWhitespace();
        if (!AtEnd() && Peek() == ']') {
            ++position_;
            return true;
        }
        while (true) {
            Value value;
            if (!ParseValue(value, error)) return false;
            output.Push(std::move(value));
            SkipWhitespace();
            if (AtEnd()) return Fail("unterminated array", error);
            if (Peek() == ',') {
                ++position_;
                continue;
            }
            if (Peek() == ']') {
                ++position_;
                return true;
            }
            return Fail("expected ',' or ']' in array", error);
        }
    }

    bool ParseString(Value& output, std::string& error) {
        ++position_;  // consume opening quote
        std::string result;
        while (true) {
            if (AtEnd()) return Fail("unterminated string", error);
            const unsigned char character =
                static_cast<unsigned char>(text_[position_++]);
            if (character == '"') break;
            if (character == '\\') {
                if (AtEnd()) return Fail("unterminated escape", error);
                const char escape = text_[position_++];
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
                        uint32_t codepoint = 0;
                        if (!ParseHex4(codepoint, error)) return false;
                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                            if (position_ + 1 >= text_.size() ||
                                text_[position_] != '\\' ||
                                text_[position_ + 1] != 'u') {
                                return Fail("unpaired surrogate escape", error);
                            }
                            position_ += 2;
                            uint32_t low = 0;
                            if (!ParseHex4(low, error)) return false;
                            if (low < 0xDC00 || low > 0xDFFF)
                                return Fail("invalid low surrogate", error);
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                                        (low - 0xDC00);
                        }
                        AppendUtf8(codepoint, result);
                        break;
                    }
                    default:
                        return Fail("invalid escape sequence", error);
                }
                continue;
            }
            if (character < 0x20)
                return Fail("control character in string", error);
            result.push_back(static_cast<char>(character));
        }
        output = Value::MakeString(std::move(result));
        return true;
    }

    bool ParseHex4(uint32_t& output, std::string& error) {
        if (position_ + 4 > text_.size())
            return Fail("truncated unicode escape", error);
        uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = text_[position_++];
            value <<= 4;
            if (digit >= '0' && digit <= '9')
                value |= static_cast<uint32_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f')
                value |= static_cast<uint32_t>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F')
                value |= static_cast<uint32_t>(digit - 'A' + 10);
            else
                return Fail("invalid hex digit in unicode escape", error);
        }
        output = value;
        return true;
    }

    static void AppendUtf8(uint32_t codepoint, std::string& output) {
        if (codepoint <= 0x7F) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(
                static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool ParseBool(Value& output, std::string& error) {
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            output = Value::MakeBool(true);
            return true;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            output = Value::MakeBool(false);
            return true;
        }
        return Fail("invalid literal", error);
    }

    bool ParseNull(Value& output, std::string& error) {
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            output = Value::MakeNull();
            return true;
        }
        return Fail("invalid literal", error);
    }

    bool ParseNumber(Value& output, std::string& error) {
        const size_t start = position_;
        if (!AtEnd() && Peek() == '-') ++position_;
        if (AtEnd() || !std::isdigit(static_cast<unsigned char>(Peek())))
            return Fail("invalid number", error);
        if (Peek() == '0') {
            ++position_;
        } else {
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())))
                ++position_;
        }
        bool isInteger = true;
        if (!AtEnd() && Peek() == '.') {
            isInteger = false;
            ++position_;
            if (AtEnd() || !std::isdigit(static_cast<unsigned char>(Peek())))
                return Fail("invalid fractional part", error);
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())))
                ++position_;
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            isInteger = false;
            ++position_;  // consume 'e'/'E'
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) ++position_;
            const size_t exponentValueStart = position_;
            if (AtEnd() || !std::isdigit(static_cast<unsigned char>(Peek())))
                return Fail("invalid exponent", error);
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())))
                ++position_;
            const std::string exponentDigits = text_.substr(
                exponentValueStart, position_ - exponentValueStart);
            long exponentMagnitude = 0;
            try {
                exponentMagnitude = std::stol(exponentDigits);
            } catch (const std::exception&) {
                exponentMagnitude = std::numeric_limits<long>::max();
            }
            if (exponentMagnitude > kMaxDecimalExponent) {
                return Fail("number exponent is too large to be a finite value",
                            error);
            }
        }
        (void)isInteger;
        output = Value::MakeRawNumber(text_.substr(start, position_ - start));
        return true;
    }

    const std::string& text_;
    size_t position_ = 0;
};

}  // namespace

Value Value::MakeNull() { return Value(); }

Value Value::MakeBool(bool value) {
    Value result;
    result.type_ = Type::Bool;
    result.bool_ = value;
    return result;
}

Value Value::MakeInt(int64_t value) {
    Value result;
    result.type_ = Type::Number;
    result.number_text_ = std::to_string(value);
    return result;
}

Value Value::MakeDouble(double value) {
    Value result;
    result.type_ = Type::Number;
    result.number_text_ = std::to_string(value);
    return result;
}

Value Value::MakeRawNumber(std::string text) {
    Value result;
    result.type_ = Type::Number;
    result.number_text_ = std::move(text);
    return result;
}

Value Value::MakeString(std::string value) {
    Value result;
    result.type_ = Type::String;
    result.string_ = std::move(value);
    return result;
}

Value Value::MakeArray() {
    Value result;
    result.type_ = Type::Array;
    return result;
}

Value Value::MakeObject() {
    Value result;
    result.type_ = Type::Object;
    return result;
}

bool Value::AsInt64(int64_t& output) const {
    if (type_ != Type::Number) return false;
    const std::string& text = number_text_;
    if (text.empty()) return false;
    for (size_t index = (text[0] == '-') ? 1 : 0; index < text.size();
         ++index) {
        if (!std::isdigit(static_cast<unsigned char>(text[index])))
            return false;
    }
    if (text == "-" || text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end != text.c_str() + text.size()) return false;
    output = static_cast<int64_t>(parsed);
    return true;
}

const Value* Value::Find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& entry : object_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

void Value::Set(std::string key, Value value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        array_.clear();
    }
    for (auto& entry : object_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
}

void Value::Push(Value value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        object_.clear();
    }
    array_.push_back(std::move(value));
}

std::string EscapeJsonString(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 2);
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (character < 0x20) {
                    static const char kHexDigits[] = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(kHexDigits[character >> 4]);
                    output.push_back(kHexDigits[character & 0xF]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    return output;
}

void Value::DumpTo(std::string& output) const {
    switch (type_) {
        case Type::Null:
            output += "null";
            break;
        case Type::Bool:
            output += bool_ ? "true" : "false";
            break;
        case Type::Number:
            output += number_text_.empty() ? "0" : number_text_;
            break;
        case Type::String:
            output.push_back('"');
            output += EscapeJsonString(string_);
            output.push_back('"');
            break;
        case Type::Array: {
            output.push_back('[');
            for (size_t index = 0; index < array_.size(); ++index) {
                if (index) output.push_back(',');
                array_[index].DumpTo(output);
            }
            output.push_back(']');
            break;
        }
        case Type::Object: {
            output.push_back('{');
            for (size_t index = 0; index < object_.size(); ++index) {
                if (index) output.push_back(',');
                output.push_back('"');
                output += EscapeJsonString(object_[index].first);
                output += "\":";
                object_[index].second.DumpTo(output);
            }
            output.push_back('}');
            break;
        }
    }
}

std::string Value::Dump() const {
    std::string output;
    DumpTo(output);
    return output;
}

bool Value::Parse(const std::string& text, Value& output, std::string& error) {
    Parser parser(text);
    return parser.ParseDocument(output, error);
}

}  // namespace mcp_json

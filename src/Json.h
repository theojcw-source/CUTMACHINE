#pragma once

// A small, general-purpose JSON value tree used only at the MCP HTTP/
// JSON-RPC boundary (see McpServer.h/McpTools.h). This is deliberately not
// shared with Operations.cc's internal reader/writer: that one serializes a
// fixed, positional field order for Operation/EditLog values and is not a
// general parser. This one accepts arbitrary key order, so it can validate
// caller-supplied tool arguments and reject unknown keys.
//
// Numbers are kept as their exact source text rather than converted to a
// double. Callers that need a time value must go through AsInt64(), which
// requires a plain integer literal (no '.', no exponent) and fails
// otherwise -- this is the one place a JSON boundary value becomes a
// RationalTime component, and it never rounds silently (PHILOSOPHY.md
// principle 4). Parse() also rejects any number literal whose exponent
// would overflow to a non-finite value, naming the offending path.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mcp_json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;

    static Value MakeNull();
    static Value MakeBool(bool value);
    static Value MakeInt(int64_t value);
    static Value MakeDouble(double value);
    static Value MakeString(std::string value);
    static Value MakeArray();
    static Value MakeObject();
    // For the parser: text must already be a validated JSON number literal.
    static Value MakeRawNumber(std::string text);

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool() const { return bool_; }
    const std::string& AsString() const { return string_; }
    const std::vector<Value>& AsArray() const { return array_; }
    const std::vector<std::pair<std::string, Value>>& AsObject() const {
        return object_;
    }

    // Strict integer extraction. Fails (leaving output untouched) unless the
    // original literal contains only an optional sign and digits, and fits
    // int64_t exactly.
    bool AsInt64(int64_t& output) const;

    // Object lookup by key. Returns nullptr if this is not an object or the
    // key is absent. O(n) -- argument objects are always small.
    const Value* Find(const std::string& key) const;

    // Mutators used only when building responses (never on parsed input).
    void Set(std::string key, Value value);
    void Push(Value value);

    std::string Dump() const;

    static bool Parse(const std::string& text, Value& output,
                      std::string& error);

private:
    void DumpTo(std::string& output) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    std::string number_text_;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

std::string EscapeJsonString(const std::string& input);

}  // namespace mcp_json

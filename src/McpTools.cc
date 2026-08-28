#include "McpTools.h"

#include "Document.h"
#include "EditLog.h"
#include "RationalTime.h"
#include "Subtitles.h"
#include "Ulid.h"

#include <algorithm>
#include <limits>
#include <map>

namespace {

using mcp_json::Value;

// ---------------------------------------------------------------------
// JSON Schema fragment builders. Schemas are static per tool, so these are
// just string assembly -- the runtime validation that actually matters
// (unknown keys, non-finite numbers, ID resolution) lives in the ReadXxx
// helpers below and runs against the caller's arguments, not the schema.
// ---------------------------------------------------------------------

std::string Esc(const std::string& text) {
    return mcp_json::EscapeJsonString(text);
}

std::string StringSchema(const std::string& description) {
    return "{\"type\":\"string\",\"description\":\"" + Esc(description) + "\"}";
}

std::string IdSchema(const std::string& description) {
    return "{\"type\":\"string\",\"description\":\"" +
           Esc(description +
               " Accepts a full ID or any unambiguous prefix of one.") +
           "\"}";
}

std::string IdArraySchema(const std::string& description) {
    return "{\"type\":\"array\",\"items\":" + IdSchema("") +
           ",\"description\":\"" + Esc(description) + "\"}";
}

std::string BoolSchema(const std::string& description) {
    return "{\"type\":\"boolean\",\"description\":\"" + Esc(description) +
           "\"}";
}

std::string IntSchema(const std::string& description) {
    return "{\"type\":\"integer\",\"description\":\"" + Esc(description) +
           "\"}";
}

std::string PositiveIntSchema(const std::string& description) {
    return "{\"type\":\"integer\",\"minimum\":1,\"description\":\"" +
           Esc(description) + "\"}";
}

std::string EnumSchema(const std::vector<std::string>& values,
                       const std::string& description) {
    std::string valuesJson = "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) valuesJson += ",";
        valuesJson += "\"" + values[index] + "\"";
    }
    valuesJson += "]";
    return "{\"type\":\"string\",\"enum\":" + valuesJson +
           ",\"description\":\"" + Esc(description) + "\"}";
}

const char kTimeSchemaText[] =
    "{\"type\":\"object\",\"description\":\"Exact RationalTime as integer "
    "value/rate (never a float); rate is ticks per second and must be a "
    "positive integer.\",\"properties\":{\"value\":{\"type\":\"integer\"},"
    "\"rate\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"value\","
    "\"rate\"],\"additionalProperties\":false}";

const char kFractionSchemaText[] =
    "{\"type\":\"object\",\"description\":\"Exact fraction as integer "
    "num/den (never a float).\",\"properties\":{\"num\":{\"type\":"
    "\"integer\"},\"den\":{\"type\":\"integer\"}},\"required\":[\"num\","
    "\"den\"],\"additionalProperties\":false}";

std::string ArraySchema(const std::string& itemSchema,
                        const std::string& description) {
    return "{\"type\":\"array\",\"items\":" + itemSchema +
           ",\"description\":\"" + Esc(description) + "\"}";
}

// Builds one tool's top-level `inputSchema` object and, incidentally, the
// list of keys CheckKnownKeys() should accept -- the two must always agree,
// so both come from the same Field() calls.
class SchemaBuilder {
public:
    SchemaBuilder& Field(const std::string& name, const std::string& schema,
                         bool required) {
        if (!properties_.empty()) properties_ += ",";
        properties_ += "\"" + name + "\":" + schema;
        if (required) required_.push_back(name);
        return *this;
    }

    std::string Build(const std::string& description) const {
        std::string requiredJson = "[";
        for (size_t index = 0; index < required_.size(); ++index) {
            if (index) requiredJson += ",";
            requiredJson += "\"" + required_[index] + "\"";
        }
        requiredJson += "]";
        return "{\"type\":\"object\",\"description\":\"" + Esc(description) +
               "\",\"properties\":{" + properties_ +
               "},\"required\":" + requiredJson +
               ",\"additionalProperties\":false}";
    }

private:
    std::string properties_;
    std::vector<std::string> required_;
};

// ---------------------------------------------------------------------
// Argument readers. Every one names the offending field ("tool.field" or
// "tool.field[index]") in `message` on failure. None of them throw.
// ---------------------------------------------------------------------

bool CheckKnownKeys(const Value& args, const std::vector<std::string>& allowed,
                    const std::string& path, std::string& message) {
    if (!args.IsObject()) {
        message = "'" + path + "' arguments must be a JSON object";
        return false;
    }
    for (const auto& entry : args.AsObject()) {
        if (std::find(allowed.begin(), allowed.end(), entry.first) ==
            allowed.end()) {
            message =
                "'" + path + "' has unknown argument '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

bool ReadString(const Value& args, const std::string& key,
                const std::string& path, bool required,
                const std::string& defaultValue, std::string& out,
                std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        out = defaultValue;
        return true;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    out = field->AsString();
    return true;
}

bool ReadBool(const Value& args, const std::string& key,
              const std::string& path, bool defaultValue, bool& out,
              std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        out = defaultValue;
        return true;
    }
    if (!field->IsBool()) {
        message = "'" + path + "." + key + "' must be a boolean";
        return false;
    }
    out = field->AsBool();
    return true;
}

bool ReadInt64(const Value& args, const std::string& key,
               const std::string& path, bool required, int64_t defaultValue,
               int64_t& out, std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        out = defaultValue;
        return true;
    }
    if (!field->IsNumber() || !field->AsInt64(out)) {
        message = "'" + path + "." + key +
                  "' must be an exact integer (no fraction, no exponent)";
        return false;
    }
    return true;
}

bool ReadInt32(const Value& args, const std::string& key,
               const std::string& path, bool required, int32_t defaultValue,
               int32_t& out, std::string& message) {
    int64_t wide = defaultValue;
    if (!ReadInt64(args, key, path, required, defaultValue, wide, message))
        return false;
    if (wide < std::numeric_limits<int32_t>::min() ||
        wide > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + "." + key + "' is out of 32-bit range";
        return false;
    }
    out = static_cast<int32_t>(wide);
    return true;
}

bool ReadTimeValue(const Value& field, const std::string& path,
                   RationalTime& out, std::string& message) {
    if (!field.IsObject()) {
        message = "'" + path + "' must be an object with 'value' and 'rate'";
        return false;
    }
    for (const auto& entry : field.AsObject()) {
        if (entry.first != "value" && entry.first != "rate") {
            message = "'" + path + "' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    const Value* valueField = field.Find("value");
    const Value* rateField = field.Find("rate");
    if (!valueField || !rateField) {
        message = "'" + path + "' requires both 'value' and 'rate'";
        return false;
    }
    int64_t value = 0;
    int64_t rate = 0;
    if (!valueField->IsNumber() || !valueField->AsInt64(value)) {
        message = "'" + path + ".value' must be an exact integer";
        return false;
    }
    if (!rateField->IsNumber() || !rateField->AsInt64(rate)) {
        message = "'" + path + ".rate' must be an exact integer";
        return false;
    }
    if (rate <= 0 || rate > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + ".rate' must be a positive 32-bit integer";
        return false;
    }
    out = RationalTime{value, static_cast<int32_t>(rate)};
    return true;
}

bool ReadTime(const Value& args, const std::string& key,
              const std::string& path, bool required, RationalTime& out,
              std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        return true;
    }
    return ReadTimeValue(*field, path + "." + key, out, message);
}

bool ReadFractionValue(const Value& field, const std::string& path,
                       int32_t& num, int32_t& den, std::string& message) {
    if (!field.IsObject()) {
        message = "'" + path + "' must be an object with 'num' and 'den'";
        return false;
    }
    for (const auto& entry : field.AsObject()) {
        if (entry.first != "num" && entry.first != "den") {
            message = "'" + path + "' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    const Value* numField = field.Find("num");
    const Value* denField = field.Find("den");
    if (!numField || !denField) {
        message = "'" + path + "' requires both 'num' and 'den'";
        return false;
    }
    int64_t numWide = 0;
    int64_t denWide = 0;
    if (!numField->IsNumber() || !numField->AsInt64(numWide)) {
        message = "'" + path + ".num' must be an exact integer";
        return false;
    }
    if (!denField->IsNumber() || !denField->AsInt64(denWide)) {
        message = "'" + path + ".den' must be an exact integer";
        return false;
    }
    if (denWide == 0 || denWide < std::numeric_limits<int32_t>::min() ||
        denWide > std::numeric_limits<int32_t>::max() ||
        numWide < std::numeric_limits<int32_t>::min() ||
        numWide > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + "' must have a non-zero 32-bit 'den'";
        return false;
    }
    num = static_cast<int32_t>(numWide);
    den = static_cast<int32_t>(denWide);
    return true;
}

bool ReadId(const Value& args, const std::string& key, const std::string& path,
            const IdResolver& resolver, bool required, Ulid& out,
            std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        return true;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    return resolver.Resolve(path + "." + key, field->AsString(), out, message);
}

// An optional ID that may be explicitly empty ("no parent bin", "clear
// caption"): an empty string is accepted as-is, never resolved.
bool ReadOptionalIdOrEmpty(const Value& args, const std::string& key,
                           const std::string& path, const IdResolver& resolver,
                           Ulid& out, std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        out.clear();
        return true;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    if (field->AsString().empty()) {
        out.clear();
        return true;
    }
    return resolver.Resolve(path + "." + key, field->AsString(), out, message);
}

bool ReadIdArray(const Value& args, const std::string& key,
                 const std::string& path, const IdResolver& resolver,
                 std::vector<Ulid>& out, std::string& message) {
    const Value* field = args.Find(key);
    out.clear();
    if (!field) return true;
    if (!field->IsArray()) {
        message = "'" + path + "." + key + "' must be an array";
        return false;
    }
    for (size_t index = 0; index < field->AsArray().size(); ++index) {
        const Value& item = field->AsArray()[index];
        const std::string itemPath =
            path + "." + key + "[" + std::to_string(index) + "]";
        if (!item.IsString()) {
            message = "'" + itemPath + "' must be a string";
            return false;
        }
        Ulid resolved;
        if (!resolver.Resolve(itemPath, item.AsString(), resolved, message))
            return false;
        out.push_back(resolved);
    }
    return true;
}

bool ReadEdge(const Value& args, const std::string& key,
              const std::string& path, TrimEdge& out, std::string& message) {
    std::string text;
    if (!ReadString(args, key, path, true, "", text, message)) return false;
    if (text == "Head") {
        out = TrimEdge::Head;
        return true;
    }
    if (text == "Tail") {
        out = TrimEdge::Tail;
        return true;
    }
    message = "'" + path + "." + key + "' must be 'Head' or 'Tail'";
    return false;
}

bool ReadAlignment(const Value& args, const std::string& key,
                   const std::string& path, TransitionAlignment& out,
                   std::string& message) {
    std::string text;
    if (!ReadString(args, key, path, false, "Center", text, message))
        return false;
    if (text == "Center") {
        out = TransitionAlignment::Center;
        return true;
    }
    if (text == "StartAtCut") {
        out = TransitionAlignment::StartAtCut;
        return true;
    }
    if (text == "EndAtCut") {
        out = TransitionAlignment::EndAtCut;
        return true;
    }
    message = "'" + path + "." + key +
              "' must be one of 'Center', 'StartAtCut', 'EndAtCut'";
    return false;
}

bool Fail(std::string& errorName, std::string& message, std::string text) {
    errorName = "ValidationFailed";
    message = std::move(text);
    return false;
}

// ---------------------------------------------------------------------
// Per-tool dispatch. Each function validates, resolves IDs, builds the
// matching Operations.h struct, and ends by handing it to `backend` --
// which is EditLog::Apply (in-memory backend) or ApplyOperationCommand
// (project-file backend), never a hand-rolled mutation.
// ---------------------------------------------------------------------

bool DispatchInsertClip(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "track_id", "source_id", "source_in", "duration", "timeline_in"};
    if (!CheckKnownKeys(args, kAllowed, "insert_clip", message))
        return Fail(errorName, message, message);
    InsertClipOperation op;
    if (!ReadId(args, "track_id", "insert_clip", resolver, true, op.track_id,
                message) ||
        !ReadId(args, "source_id", "insert_clip", resolver, true, op.source_id,
                message) ||
        !ReadTime(args, "source_in", "insert_clip", true, op.source_in,
                  message) ||
        !ReadTime(args, "duration", "insert_clip", true, op.duration,
                  message) ||
        !ReadTime(args, "timeline_in", "insert_clip", true, op.timeline_in,
                  message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveClip(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_clip", message))
        return Fail(errorName, message, message);
    RemoveClipOperation op;
    if (!ReadId(args, "clip_id", "remove_clip", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchClearClip(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id"};
    if (!CheckKnownKeys(args, kAllowed, "clear_clip", message))
        return Fail(errorName, message, message);
    ClearClipOperation op;
    if (!ReadId(args, "clip_id", "clear_clip", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchClearClips(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "clear_clips", message))
        return Fail(errorName, message, message);
    ClearClipsOperation op;
    if (!ReadIdArray(args, "clip_ids", "clear_clips", resolver, op.clip_ids,
                     message))
        return Fail(errorName, message, message);
    if (op.clip_ids.empty()) {
        message = "'clear_clips.clip_ids' must contain at least one clip";
        return Fail(errorName, message, message);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchTrimClip(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "edge",
                                                      "delta"};
    if (!CheckKnownKeys(args, kAllowed, "trim_clip", message))
        return Fail(errorName, message, message);
    TrimClipOperation op;
    if (!ReadId(args, "clip_id", "trim_clip", resolver, true, op.clip_id,
                message) ||
        !ReadEdge(args, "edge", "trim_clip", op.edge, message) ||
        !ReadTime(args, "delta", "trim_clip", true, op.delta, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveClip(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "track_id",
                                                      "timeline_in"};
    if (!CheckKnownKeys(args, kAllowed, "move_clip", message))
        return Fail(errorName, message, message);
    MoveClipOperation op;
    if (!ReadId(args, "clip_id", "move_clip", resolver, true, op.clip_id,
                message) ||
        !ReadId(args, "track_id", "move_clip", resolver, true, op.track_id,
                message) ||
        !ReadTime(args, "timeline_in", "move_clip", true, op.timeline_in,
                  message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSplitClip(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id",
                                                      "timeline_position"};
    if (!CheckKnownKeys(args, kAllowed, "split_clip", message))
        return Fail(errorName, message, message);
    SplitClipOperation op;
    if (!ReadId(args, "clip_id", "split_clip", resolver, true, op.clip_id,
                message) ||
        !ReadTime(args, "timeline_position", "split_clip", true,
                  op.timeline_position, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSplitAtInterval(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "interval"};
    if (!CheckKnownKeys(args, kAllowed, "split_at_interval", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    RationalTime interval;
    if (!ReadId(args, "clip_id", "split_at_interval", resolver, true, clipId,
                message) ||
        !ReadTime(args, "interval", "split_at_interval", true, interval,
                  message))
        return Fail(errorName, message, message);

    // Its own snapshot: dispatch functions receive only the ID resolver, and
    // giving all 38 of them a Document parameter to serve the one tool that
    // resolves against clip geometry is the wrong trade. Nothing mutates
    // between McpToolRegistry::Call's snapshot and this one.
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }

    SplitClipAtPositionsOperation op;
    if (!ResolveIntervalSplits(document, clipId, interval, op, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchDeleteGap(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "track_id", "gap_start", "gap_duration", "linked_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "delete_gap", message))
        return Fail(errorName, message, message);
    DeleteGapOperation op;
    if (!ReadId(args, "track_id", "delete_gap", resolver, true, op.track_id,
                message) ||
        !ReadTime(args, "gap_start", "delete_gap", true, op.gap_start,
                  message) ||
        !ReadTime(args, "gap_duration", "delete_gap", true, op.gap_duration,
                  message) ||
        !ReadIdArray(args, "linked_track_ids", "delete_gap", resolver,
                     op.linked_track_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchDetachAudio(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"video_clip_id",
                                                      "audio_track_id"};
    if (!CheckKnownKeys(args, kAllowed, "detach_audio", message))
        return Fail(errorName, message, message);
    DetachAudioOperation op;
    if (!ReadId(args, "video_clip_id", "detach_audio", resolver, true,
                op.video_clip_id, message) ||
        !ReadId(args, "audio_track_id", "detach_audio", resolver, true,
                op.audio_track_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveLinkedClips(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id", "moves"};
    if (!CheckKnownKeys(args, kAllowed, "move_linked_clips", message))
        return Fail(errorName, message, message);
    MoveLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "move_linked_clips", resolver, true,
                op.link_group_id, message))
        return Fail(errorName, message, message);
    const Value* moves = args.Find("moves");
    if (!moves || !moves->IsArray() || moves->AsArray().empty())
        return Fail(errorName, message,
                    "'move_linked_clips.moves' must be a non-empty array");
    for (size_t index = 0; index < moves->AsArray().size(); ++index) {
        const Value& item = moves->AsArray()[index];
        const std::string path =
            "move_linked_clips.moves[" + std::to_string(index) + "]";
        static const std::vector<std::string> kMoveKeys = {
            "clip_id", "track_id", "timeline_in"};
        if (!CheckKnownKeys(item, kMoveKeys, path, message))
            return Fail(errorName, message, message);
        LinkedClipMove move;
        if (!ReadId(item, "clip_id", path, resolver, true, move.clip_id,
                    message) ||
            !ReadId(item, "track_id", path, resolver, true, move.track_id,
                    message) ||
            !ReadTime(item, "timeline_in", path, true, move.timeline_in,
                      message))
            return Fail(errorName, message, message);
        op.moves.push_back(move);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveClips(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"moves"};
    if (!CheckKnownKeys(args, kAllowed, "move_clips", message))
        return Fail(errorName, message, message);
    MoveClipsOperation op;
    const Value* moves = args.Find("moves");
    if (!moves || !moves->IsArray() || moves->AsArray().size() < 2)
        return Fail(errorName, message,
                    "'move_clips.moves' must contain at least two items");
    for (size_t index = 0; index < moves->AsArray().size(); ++index) {
        const Value& item = moves->AsArray()[index];
        const std::string path =
            "move_clips.moves[" + std::to_string(index) + "]";
        static const std::vector<std::string> kMoveKeys = {
            "clip_id", "track_id", "timeline_in"};
        if (!CheckKnownKeys(item, kMoveKeys, path, message))
            return Fail(errorName, message, message);
        LinkedClipMove move;
        if (!ReadId(item, "clip_id", path, resolver, true, move.clip_id,
                    message) ||
            !ReadId(item, "track_id", path, resolver, true, move.track_id,
                    message) ||
            !ReadTime(item, "timeline_in", path, true, move.timeline_in,
                      message))
            return Fail(errorName, message, message);
        op.moves.push_back(move);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchTrimLinkedClips(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id", "trims"};
    if (!CheckKnownKeys(args, kAllowed, "trim_linked_clips", message))
        return Fail(errorName, message, message);
    TrimLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "trim_linked_clips", resolver, true,
                op.link_group_id, message))
        return Fail(errorName, message, message);
    const Value* trims = args.Find("trims");
    if (!trims || !trims->IsArray() || trims->AsArray().empty())
        return Fail(errorName, message,
                    "'trim_linked_clips.trims' must be a non-empty array");
    for (size_t index = 0; index < trims->AsArray().size(); ++index) {
        const Value& item = trims->AsArray()[index];
        const std::string path =
            "trim_linked_clips.trims[" + std::to_string(index) + "]";
        static const std::vector<std::string> kTrimKeys = {"clip_id", "edge",
                                                           "delta"};
        if (!CheckKnownKeys(item, kTrimKeys, path, message))
            return Fail(errorName, message, message);
        LinkedClipTrim trim;
        if (!ReadId(item, "clip_id", path, resolver, true, trim.clip_id,
                    message) ||
            !ReadEdge(item, "edge", path, trim.edge, message) ||
            !ReadTime(item, "delta", path, true, trim.delta, message))
            return Fail(errorName, message, message);
        op.trims.push_back(trim);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchShortenLinkedClip(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "edge", "amount", "unit", "preview"};
    if (!CheckKnownKeys(args, kAllowed, "shorten_linked_clip", message))
        return Fail(errorName, message, message);

    Ulid clipId;
    TrimEdge edge = TrimEdge::Tail;
    int64_t amount = 0;
    std::string unit;
    bool preview = false;
    if (!ReadId(args, "clip_id", "shorten_linked_clip", resolver, true, clipId,
                message) ||
        !ReadEdge(args, "edge", "shorten_linked_clip", edge, message) ||
        !ReadInt64(args, "amount", "shorten_linked_clip", true, 0, amount,
                   message) ||
        !ReadString(args, "unit", "shorten_linked_clip", true, "", unit,
                    message) ||
        !ReadBool(args, "preview", "shorten_linked_clip", false, preview,
                  message))
        return Fail(errorName, message, message);
    if (amount <= 0)
        return Fail(errorName, message,
                    "'shorten_linked_clip.amount' must be positive");

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* anchor = document.FindClip(clipId);
    if (!anchor)
        return Fail(errorName, message,
                    "'shorten_linked_clip.clip_id' is not a clip");
    if (anchor->link_group_id.empty())
        return Fail(errorName, message,
                    "'shorten_linked_clip.clip_id' is not A/V-linked");

    RationalTime magnitude;
    if (unit == "Seconds") {
        magnitude = {amount, 1};
    } else if (unit == "Frames") {
        if (document.sequence.frame_rate.num <= 0 ||
            document.sequence.frame_rate.den <= 0)
            return Fail(errorName, message,
                        "the sequence frame rate must be positive");
        const __int128 ticks =
            static_cast<__int128>(amount) * document.sequence.frame_rate.den;
        if (ticks > std::numeric_limits<int64_t>::max())
            return Fail(errorName, message,
                        "'shorten_linked_clip.amount' overflows RationalTime");
        magnitude = {static_cast<int64_t>(ticks),
                     document.sequence.frame_rate.num};
    } else {
        return Fail(errorName, message,
                    "'shorten_linked_clip.unit' must be 'Frames' or 'Seconds'");
    }
    if (edge == TrimEdge::Tail) magnitude.value = -magnitude.value;

    TrimLinkedClipsOperation operation;
    operation.link_group_id = anchor->link_group_id;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.link_group_id == operation.link_group_id)
                operation.trims.push_back({clip.id, edge, magnitude});
        }
    }
    if (operation.trims.size() < 2)
        return Fail(errorName, message,
                    "the linked group has fewer than two members");

    if (!preview)
        return backend.ApplyOperation(operation, resultJson, errorName,
                                      message);

    Document candidate = document;
    EditLog previewLog;
    EditError previewError = EditError::None;
    std::string previewMessage;
    Operation previewOperation = operation;
    if (!previewLog.Apply(candidate, previewOperation, previewError,
                          previewMessage)) {
        errorName = EditErrorName(previewError);
        message = previewMessage;
        return false;
    }
    resultJson = "{\"ok\":true,\"preview\":true,\"operation\":" +
                 SerializeOperation(operation) + "}";
    return true;
}

bool DispatchRippleTrim(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "edge", "delta", "linked_clip_ids", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "ripple_trim", message))
        return Fail(errorName, message, message);
    RippleTrimOperation op;
    if (!ReadId(args, "clip_id", "ripple_trim", resolver, true, op.clip_id,
                message) ||
        !ReadEdge(args, "edge", "ripple_trim", op.edge, message) ||
        !ReadTime(args, "delta", "ripple_trim", true, op.delta, message) ||
        !ReadIdArray(args, "linked_clip_ids", "ripple_trim", resolver,
                     op.linked_clip_ids, message) ||
        !ReadIdArray(args, "sync_track_ids", "ripple_trim", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRollEdit(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"pairs", "delta"};
    if (!CheckKnownKeys(args, kAllowed, "roll_edit", message))
        return Fail(errorName, message, message);
    RollEditOperation op;
    if (!ReadTime(args, "delta", "roll_edit", true, op.delta, message))
        return Fail(errorName, message, message);
    const Value* pairs = args.Find("pairs");
    if (!pairs || !pairs->IsArray() || pairs->AsArray().empty())
        return Fail(errorName, message,
                    "'roll_edit.pairs' must be a non-empty array");
    for (size_t index = 0; index < pairs->AsArray().size(); ++index) {
        const Value& item = pairs->AsArray()[index];
        const std::string path =
            "roll_edit.pairs[" + std::to_string(index) + "]";
        static const std::vector<std::string> kPairKeys = {"left_clip_id",
                                                           "right_clip_id"};
        if (!CheckKnownKeys(item, kPairKeys, path, message))
            return Fail(errorName, message, message);
        RollEditPair pair;
        if (!ReadId(item, "left_clip_id", path, resolver, true,
                    pair.left_clip_id, message) ||
            !ReadId(item, "right_clip_id", path, resolver, true,
                    pair.right_clip_id, message))
            return Fail(errorName, message, message);
        op.pairs.push_back(pair);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSlipEdit(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids", "delta"};
    if (!CheckKnownKeys(args, kAllowed, "slip_edit", message))
        return Fail(errorName, message, message);
    SlipEditOperation op;
    if (!ReadTime(args, "delta", "slip_edit", true, op.delta, message))
        return Fail(errorName, message, message);
    if (!ReadIdArray(args, "clip_ids", "slip_edit", resolver, op.clip_ids,
                     message))
        return Fail(errorName, message, message);
    if (op.clip_ids.empty())
        return Fail(errorName, message,
                    "'slip_edit.clip_ids' must be a non-empty array");
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveLinkedClips(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id",
                                                      "clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "remove_linked_clips", message))
        return Fail(errorName, message, message);
    RemoveLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "remove_linked_clips", resolver, true,
                op.link_group_id, message) ||
        !ReadIdArray(args, "clip_ids", "remove_linked_clips", resolver,
                     op.clip_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchClearLinkedClips(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id",
                                                      "clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "clear_linked_clips", message))
        return Fail(errorName, message, message);
    ClearLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "clear_linked_clips", resolver, true,
                op.link_group_id, message) ||
        !ReadIdArray(args, "clip_ids", "clear_linked_clips", resolver,
                     op.clip_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSplitLinkedClips(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "link_group_id", "clip_ids", "timeline_position"};
    if (!CheckKnownKeys(args, kAllowed, "split_linked_clips", message))
        return Fail(errorName, message, message);
    SplitLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "split_linked_clips", resolver, true,
                op.link_group_id, message) ||
        !ReadIdArray(args, "clip_ids", "split_linked_clips", resolver,
                     op.clip_ids, message) ||
        !ReadTime(args, "timeline_position", "split_linked_clips", true,
                  op.timeline_position, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchPasteClips(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clips", "overwrite"};
    if (!CheckKnownKeys(args, kAllowed, "paste_clips", message))
        return Fail(errorName, message, message);
    PasteClipsOperation op;
    if (!ReadBool(args, "overwrite", "paste_clips", false, op.overwrite,
                  message))
        return Fail(errorName, message, message);
    const Value* clips = args.Find("clips");
    if (!clips || !clips->IsArray() || clips->AsArray().empty())
        return Fail(errorName, message,
                    "'paste_clips.clips' must be a non-empty array");

    struct Item {
        std::string label;
        Ulid track_id;
        Ulid source_id;
        RationalTime source_in;
        RationalTime duration;
        RationalTime timeline_in;
        std::string link_group;
        bool anchor = false;
    };
    std::vector<Item> items;
    static const std::vector<std::string> kItemKeys = {
        "label",    "track_id",    "source_id",  "source_in",
        "duration", "timeline_in", "link_group", "anchor"};
    for (size_t index = 0; index < clips->AsArray().size(); ++index) {
        const Value& raw = clips->AsArray()[index];
        const std::string path =
            "paste_clips.clips[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(raw, kItemKeys, path, message))
            return Fail(errorName, message, message);
        Item item;
        if (!ReadString(raw, "label", path, true, "", item.label, message) ||
            !ReadId(raw, "track_id", path, resolver, true, item.track_id,
                    message) ||
            !ReadId(raw, "source_id", path, resolver, true, item.source_id,
                    message) ||
            !ReadTime(raw, "source_in", path, true, item.source_in, message) ||
            !ReadTime(raw, "duration", path, true, item.duration, message) ||
            !ReadTime(raw, "timeline_in", path, true, item.timeline_in,
                      message) ||
            !ReadString(raw, "link_group", path, false, "", item.link_group,
                        message) ||
            !ReadBool(raw, "anchor", path, false, item.anchor, message))
            return Fail(errorName, message, message);
        if (item.label.empty())
            return Fail(errorName, message,
                        "'" + path + ".label' must not be empty");
        items.push_back(std::move(item));
    }

    std::map<std::string, Ulid> labelToCopiedId;
    for (const Item& item : items) {
        if (labelToCopiedId.count(item.label))
            return Fail(errorName, message,
                        "'paste_clips.clips' has a duplicate label '" +
                            item.label + "'");
        labelToCopiedId[item.label] = GenerateUlid();
    }
    std::map<std::string, Ulid> linkGroupUlid;
    std::map<std::string, std::string> linkGroupAnchorLabel;
    for (const Item& item : items) {
        if (item.link_group.empty()) continue;
        if (!linkGroupUlid.count(item.link_group))
            linkGroupUlid[item.link_group] = GenerateUlid();
        if (item.anchor || !linkGroupAnchorLabel.count(item.link_group))
            linkGroupAnchorLabel[item.link_group] = item.label;
    }
    for (const Item& item : items) {
        PastedClip clip;
        clip.copied_clip_id = labelToCopiedId[item.label];
        clip.track_id = item.track_id;
        clip.source_id = item.source_id;
        clip.source_in = item.source_in;
        clip.duration = item.duration;
        clip.timeline_in = item.timeline_in;
        if (!item.link_group.empty()) {
            clip.copied_link_group_id = linkGroupUlid[item.link_group];
            clip.copied_sync_anchor_clip_id =
                labelToCopiedId[linkGroupAnchorLabel[item.link_group]];
        }
        op.clips.push_back(std::move(clip));
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddTrack(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "kind", "index", "locked", "sync_lock", "visible", "muted", "solo"};
    if (!CheckKnownKeys(args, kAllowed, "add_track", message))
        return Fail(errorName, message, message);
    AddTrackOperation op;
    int64_t index = -1;
    if (!ReadString(args, "kind", "add_track", true, "", op.kind, message) ||
        !ReadInt64(args, "index", "add_track", true, -1, index, message) ||
        !ReadBool(args, "locked", "add_track", false, op.locked, message) ||
        !ReadBool(args, "sync_lock", "add_track", true, op.sync_lock,
                  message) ||
        !ReadBool(args, "visible", "add_track", true, op.visible, message) ||
        !ReadBool(args, "muted", "add_track", false, op.muted, message) ||
        !ReadBool(args, "solo", "add_track", false, op.solo, message))
        return Fail(errorName, message, message);
    if (index < std::numeric_limits<int32_t>::min() ||
        index > std::numeric_limits<int32_t>::max())
        return Fail(errorName, message, "'add_track.index' is out of range");
    op.index = static_cast<int32_t>(index);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchImportSrt(McpBackend& backend, const IdResolver&,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"path", "index"};
    if (!CheckKnownKeys(args, kAllowed, "import_srt", message))
        return Fail(errorName, message, message);
    std::string path;
    int64_t index = -1;
    if (!ReadString(args, "path", "import_srt", true, "", path, message) ||
        !ReadInt64(args, "index", "import_srt", false, -1, index, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    if (index < 0) {
        index = 0;
        for (const DocumentTrack& track : document.sequence.tracks)
            index = std::max(index, static_cast<int64_t>(track.index) + 1);
    }
    if (index > std::numeric_limits<int32_t>::max())
        return Fail(errorName, message, "'import_srt.index' is out of range");
    std::vector<SubtitleCue> cues;
    if (!LoadSrt(path, cues, message)) return Fail(errorName, message, message);
    return backend.ApplyOperation(
        BuildSubtitleTrackEdit(cues, static_cast<int32_t>(index)), resultJson,
        errorName, message);
}

bool DispatchRemoveTrack(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_track", message))
        return Fail(errorName, message, message);
    RemoveTrackOperation op;
    if (!ReadId(args, "track_id", "remove_track", resolver, true, op.track_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetTrackLock(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id", "locked"};
    if (!CheckKnownKeys(args, kAllowed, "set_track_lock", message))
        return Fail(errorName, message, message);
    SetTrackLockOperation op;
    if (!ReadId(args, "track_id", "set_track_lock", resolver, true, op.track_id,
                message) ||
        !ReadBool(args, "locked", "set_track_lock", false, op.locked, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetTrackSyncLock(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id", "sync_lock"};
    if (!CheckKnownKeys(args, kAllowed, "set_track_sync_lock", message))
        return Fail(errorName, message, message);
    SetTrackSyncLockOperation op;
    if (!ReadId(args, "track_id", "set_track_sync_lock", resolver, true,
                op.track_id, message) ||
        !ReadBool(args, "sync_lock", "set_track_sync_lock", true, op.sync_lock,
                  message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetTrackOutput(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id", "visible",
                                                      "muted", "solo"};
    if (!CheckKnownKeys(args, kAllowed, "set_track_output", message))
        return Fail(errorName, message, message);
    SetTrackOutputOperation op;
    if (!ReadId(args, "track_id", "set_track_output", resolver, true,
                op.track_id, message) ||
        !ReadBool(args, "visible", "set_track_output", true, op.visible,
                  message) ||
        !ReadBool(args, "muted", "set_track_output", false, op.muted,
                  message) ||
        !ReadBool(args, "solo", "set_track_output", false, op.solo, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchUpdateSequence(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"name", "width", "height",
                                                      "frame_rate"};
    if (!CheckKnownKeys(args, kAllowed, "update_sequence", message))
        return Fail(errorName, message, message);
    UpdateSequenceOperation op;
    op.sequence_id = resolver.SequenceId();
    if (!ReadString(args, "name", "update_sequence", true, "", op.name,
                    message) ||
        !ReadInt32(args, "width", "update_sequence", true, op.width, op.width,
                   message) ||
        !ReadInt32(args, "height", "update_sequence", true, op.height,
                   op.height, message))
        return Fail(errorName, message, message);
    const Value* frameRate = args.Find("frame_rate");
    if (!frameRate)
        return Fail(errorName, message,
                    "'update_sequence.frame_rate' is required");
    if (!ReadFractionValue(*frameRate, "update_sequence.frame_rate",
                           op.frame_rate.num, op.frame_rate.den, message))
        return Fail(errorName, message, message);
    if (op.frame_rate.den <= 0 || op.frame_rate.num <= 0)
        return Fail(errorName, message,
                    "'update_sequence.frame_rate' must be positive");
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetColorManagement(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    (void)resolver;
    static const std::vector<std::string> kAllowed = {
        "enabled",     "input_gamut",   "input_transfer", "input_ycbcr_matrix",
        "input_range", "working_gamut", "output_gamut",   "output_transfer"};
    if (!CheckKnownKeys(args, kAllowed, "set_color_management", message))
        return Fail(errorName, message, message);
    SetColorManagementOperation op;
    if (!ReadBool(args, "enabled", "set_color_management", false,
                  op.settings.enabled, message) ||
        !ReadString(args, "input_gamut", "set_color_management", true, "",
                    op.settings.input_gamut, message) ||
        !ReadString(args, "input_transfer", "set_color_management", true, "",
                    op.settings.input_transfer, message) ||
        !ReadString(args, "input_ycbcr_matrix", "set_color_management", true,
                    "", op.settings.input_ycbcr_matrix, message) ||
        !ReadString(args, "input_range", "set_color_management", true, "",
                    op.settings.input_range, message) ||
        !ReadString(args, "working_gamut", "set_color_management", true, "",
                    op.settings.working_gamut, message) ||
        !ReadString(args, "output_gamut", "set_color_management", true, "",
                    op.settings.output_gamut, message) ||
        !ReadString(args, "output_transfer", "set_color_management", true, "",
                    op.settings.output_transfer, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddBin(McpBackend& backend, const IdResolver& resolver,
                    const Value& args, std::string& resultJson,
                    std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"name", "parent_id"};
    if (!CheckKnownKeys(args, kAllowed, "add_bin", message))
        return Fail(errorName, message, message);
    AddBinOperation op;
    if (!ReadString(args, "name", "add_bin", true, "", op.name, message) ||
        !ReadOptionalIdOrEmpty(args, "parent_id", "add_bin", resolver,
                               op.parent_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveBin(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"bin_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_bin", message))
        return Fail(errorName, message, message);
    RemoveBinOperation op;
    if (!ReadId(args, "bin_id", "remove_bin", resolver, true, op.bin_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRenameBin(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"bin_id", "name"};
    if (!CheckKnownKeys(args, kAllowed, "rename_bin", message))
        return Fail(errorName, message, message);
    RenameBinOperation op;
    if (!ReadId(args, "bin_id", "rename_bin", resolver, true, op.bin_id,
                message) ||
        !ReadString(args, "name", "rename_bin", true, "", op.name, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveBin(McpBackend& backend, const IdResolver& resolver,
                     const Value& args, std::string& resultJson,
                     std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"bin_id", "parent_id"};
    if (!CheckKnownKeys(args, kAllowed, "move_bin", message))
        return Fail(errorName, message, message);
    MoveBinOperation op;
    if (!ReadId(args, "bin_id", "move_bin", resolver, true, op.bin_id,
                message) ||
        !ReadOptionalIdOrEmpty(args, "parent_id", "move_bin", resolver,
                               op.parent_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetMediaBin(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id", "bin_id"};
    if (!CheckKnownKeys(args, kAllowed, "set_media_bin", message))
        return Fail(errorName, message, message);
    SetMediaBinOperation op;
    if (!ReadId(args, "media_id", "set_media_bin", resolver, true, op.media_id,
                message) ||
        !ReadOptionalIdOrEmpty(args, "bin_id", "set_media_bin", resolver,
                               op.bin_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddMarker(McpBackend& backend, const IdResolver&,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "name", "time", "color", "category", "insertion_index"};
    if (!CheckKnownKeys(args, kAllowed, "add_marker", message))
        return Fail(errorName, message, message);
    AddMarkerOperation op;
    int64_t insertionIndex = -1;
    if (!ReadString(args, "name", "add_marker", true, "", op.marker.name,
                    message) ||
        !ReadTime(args, "time", "add_marker", true, op.marker.time, message) ||
        !ReadString(args, "color", "add_marker", false, op.marker.color,
                    op.marker.color, message) ||
        !ReadString(args, "category", "add_marker", false, op.marker.category,
                    op.marker.category, message) ||
        !ReadInt64(args, "insertion_index", "add_marker", false, -1,
                   insertionIndex, message))
        return Fail(errorName, message, message);
    op.insertion_index = insertionIndex;
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveMarker(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"marker_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_marker", message))
        return Fail(errorName, message, message);
    RemoveMarkerOperation op;
    if (!ReadId(args, "marker_id", "remove_marker", resolver, true,
                op.marker_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchUpdateMarker(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "marker_id", "name", "time", "color", "category"};
    if (!CheckKnownKeys(args, kAllowed, "update_marker", message))
        return Fail(errorName, message, message);
    UpdateMarkerOperation op;
    if (!ReadId(args, "marker_id", "update_marker", resolver, true,
                op.marker_id, message) ||
        !ReadString(args, "name", "update_marker", true, "", op.name,
                    message) ||
        !ReadTime(args, "time", "update_marker", true, op.time, message) ||
        !ReadString(args, "color", "update_marker", true, "", op.color,
                    message) ||
        !ReadString(args, "category", "update_marker", true, "", op.category,
                    message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddTransition(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "track_id", "left_clip_id", "right_clip_id",  "type",
        "duration", "alignment",    "insertion_index"};
    if (!CheckKnownKeys(args, kAllowed, "add_transition", message))
        return Fail(errorName, message, message);
    AddTransitionOperation op;
    int64_t insertionIndex = -1;
    if (!ReadId(args, "track_id", "add_transition", resolver, true,
                op.transition.track_id, message) ||
        !ReadId(args, "left_clip_id", "add_transition", resolver, true,
                op.transition.left_clip_id, message) ||
        !ReadId(args, "right_clip_id", "add_transition", resolver, true,
                op.transition.right_clip_id, message) ||
        !ReadString(args, "type", "add_transition", false, op.transition.type,
                    op.transition.type, message) ||
        !ReadTime(args, "duration", "add_transition", true,
                  op.transition.duration, message) ||
        !ReadAlignment(args, "alignment", "add_transition",
                       op.transition.alignment, message) ||
        !ReadInt64(args, "insertion_index", "add_transition", false, -1,
                   insertionIndex, message))
        return Fail(errorName, message, message);
    op.insertion_index = insertionIndex;
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveTransition(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"transition_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_transition", message))
        return Fail(errorName, message, message);
    RemoveTransitionOperation op;
    if (!ReadId(args, "transition_id", "remove_transition", resolver, true,
                op.transition_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchUpdateTransition(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"transition_id", "type",
                                                      "duration", "alignment"};
    if (!CheckKnownKeys(args, kAllowed, "update_transition", message))
        return Fail(errorName, message, message);
    UpdateTransitionOperation op;
    if (!ReadId(args, "transition_id", "update_transition", resolver, true,
                op.transition_id, message) ||
        !ReadString(args, "type", "update_transition", false, op.type, op.type,
                    message) ||
        !ReadTime(args, "duration", "update_transition", true, op.duration,
                  message) ||
        !ReadAlignment(args, "alignment", "update_transition", op.alignment,
                       message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetClipLink(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "first_clip_id", "second_clip_id", "linked"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_link", message))
        return Fail(errorName, message, message);
    SetClipLinkOperation op;
    bool linked = true;
    if (!ReadId(args, "first_clip_id", "set_clip_link", resolver, true,
                op.first_clip_id, message) ||
        !ReadId(args, "second_clip_id", "set_clip_link", resolver, true,
                op.second_clip_id, message) ||
        !ReadBool(args, "linked", "set_clip_link", true, linked, message))
        return Fail(errorName, message, message);
    if (linked) {
        const Ulid group = GenerateUlid();
        op.first_group_id = group;
        op.second_group_id = group;
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetClipEffects(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "effects"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_effects", message))
        return Fail(errorName, message, message);
    SetClipEffectsOperation op;
    if (!ReadId(args, "clip_id", "set_clip_effects", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* effects = args.Find("effects");
    if (!effects || !effects->IsArray())
        return Fail(errorName, message,
                    "'set_clip_effects.effects' must be an array");
    static const std::vector<std::string> kEffectKeys = {"type", "params"};
    for (size_t index = 0; index < effects->AsArray().size(); ++index) {
        const Value& raw = effects->AsArray()[index];
        const std::string path =
            "set_clip_effects.effects[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(raw, kEffectKeys, path, message))
            return Fail(errorName, message, message);
        ClipEffect effect;
        effect.id.clear();
        if (!ReadString(raw, "type", path, true, "", effect.type, message))
            return Fail(errorName, message, message);
        const Value* params = raw.Find("params");
        if (params) {
            if (!params->IsObject())
                return Fail(errorName, message,
                            "'" + path + ".params' must be an object");
            for (const auto& entry : params->AsObject()) {
                EffectParamValue value;
                if (!ReadFractionValue(entry.second,
                                       path + ".params." + entry.first,
                                       value.num, value.den, message))
                    return Fail(errorName, message, message);
                effect.params[entry.first] = value;
            }
        }
        op.effects.push_back(std::move(effect));
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetClipOpacity(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "opacity"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_opacity", message))
        return Fail(errorName, message, message);
    SetClipOpacityOperation op;
    if (!ReadId(args, "clip_id", "set_clip_opacity", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* opacity = args.Find("opacity");
    if (!opacity || !ReadFractionValue(*opacity, "set_clip_opacity.opacity",
                                       op.opacity.num, op.opacity.den, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddCaptionStyle(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "font_family", "font_size", "color", "position", "insertion_index"};
    if (!CheckKnownKeys(args, kAllowed, "add_caption_style", message))
        return Fail(errorName, message, message);
    AddCaptionStyleOperation op;
    int64_t insertionIndex = -1;
    if (!ReadString(args, "font_family", "add_caption_style", false,
                    op.style.font_family, op.style.font_family, message) ||
        !ReadInt32(args, "font_size", "add_caption_style", false,
                   op.style.font_size, op.style.font_size, message) ||
        !ReadString(args, "color", "add_caption_style", false, op.style.color,
                    op.style.color, message) ||
        !ReadString(args, "position", "add_caption_style", false,
                    op.style.position, op.style.position, message) ||
        !ReadInt64(args, "insertion_index", "add_caption_style", false, -1,
                   insertionIndex, message))
        return Fail(errorName, message, message);
    op.insertion_index = insertionIndex;
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveCaptionStyle(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"style_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_caption_style", message))
        return Fail(errorName, message, message);
    RemoveCaptionStyleOperation op;
    if (!ReadId(args, "style_id", "remove_caption_style", resolver, true,
                op.style_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetClipCaption(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "caption_group_id", "caption_text"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_caption", message))
        return Fail(errorName, message, message);
    SetClipCaptionOperation op;
    if (!ReadId(args, "clip_id", "set_clip_caption", resolver, true, op.clip_id,
                message) ||
        !ReadOptionalIdOrEmpty(args, "caption_group_id", "set_clip_caption",
                               resolver, op.caption_group_id, message) ||
        !ReadString(args, "caption_text", "set_clip_caption", false,
                    op.caption_text, op.caption_text, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchDescribe(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    if (!CheckKnownKeys(args, {}, "describe", message))
        return Fail(errorName, message, message);
    std::string detail;
    if (!backend.Describe(resultJson, detail)) {
        errorName = "IoError";
        message = detail;
        return false;
    }
    return true;
}

bool DispatchGetTimelineTranscript(McpBackend& backend, const IdResolver&,
                                   const Value& args, std::string& resultJson,
                                   std::string& errorName,
                                   std::string& message) {
    if (!CheckKnownKeys(args, {}, "get_timeline_transcript", message))
        return Fail(errorName, message, message);
    if (!backend.ReadTimelineTranscript(resultJson, message)) {
        errorName = "IoError";
        return false;
    }
    return true;
}

// ALPHA-2026-08 -- both word tools resolve the clip, then its source's
// cached transcript, then hand word indices to ResolveWordRemoval. The model
// never sees or supplies a timecode: it names words, CUTMACHINE names frames
// (PHILOSOPHY.md principle 7).
bool ResolveClipAndTranscript(McpBackend& backend, const Ulid& clipId,
                              Document& document, const DocumentClip*& clip,
                              Transcript& transcript, std::string& errorName,
                              std::string& message) {
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    clip = document.FindClip(clipId);
    if (clip == nullptr) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + clipId + "'";
        return false;
    }
    if (!backend.ReadSourceTranscript(clip->source_id, transcript, message)) {
        errorName = "IoError";
        message = "no cached transcript for source '" + clip->source_id +
                  "': " + message;
        return false;
    }
    return true;
}

// Other clips sharing this one's A/V link group *and* genuinely covering
// the same cut. A word-level cut that touched only the clip it was aimed at
// would shorten the sound and leave the picture, so a detached A/V pair has
// to be cut together -- and working out which clips those are is a
// document-shape question the caller should not have to answer
// (PHILOSOPHY.md principle 7).
//
// The containment test is what keeps this an intent-layer decision rather
// than a guess. A link group can hold a member that reads from another
// source, or from another part of the same one; cutting it by these ranges
// would be meaningless, so it is left alone here. RemoveWordsOperation stays
// strict about whatever it is finally told: naming such a clip explicitly is
// still an error, it just is not one this resolver produces.
std::vector<Ulid> LinkedClipIdsFor(
    const Document& document, const DocumentClip& clip,
    const std::vector<WordRemovalRange>& ranges) {
    std::vector<Ulid> linked;
    if (clip.link_group_id.empty()) return linked;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& other : track.clips) {
            if (other.id == clip.id ||
                other.link_group_id != clip.link_group_id ||
                other.source_id != clip.source_id)
                continue;
            const RationalTime sourceEnd = other.source_in.add(other.duration);
            const bool covers = std::all_of(
                ranges.begin(), ranges.end(),
                [&](const WordRemovalRange& range) {
                    return range.source_start.compare(other.source_in) >= 0 &&
                           range.source_end.compare(sourceEnd) <= 0;
                });
            if (covers) linked.push_back(other.id);
        }
    }
    return linked;
}

bool DispatchListDisfluencies(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id"};
    if (!CheckKnownKeys(args, kAllowed, "list_disfluencies", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    if (!ReadId(args, "clip_id", "list_disfluencies", resolver, true, clipId,
                message))
        return Fail(errorName, message, message);
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!ResolveClipAndTranscript(backend, clipId, document, clip, transcript,
                                  errorName, message))
        return false;

    const std::vector<Disfluency> found =
        FindDisfluenciesInClip(*clip, transcript);
    Value root = Value::MakeObject();
    root.Set("clip_id", Value::MakeString(clip->id));
    root.Set("source_id", Value::MakeString(clip->source_id));
    // Reported because it bounds what this list can contain at all: Whisper's
    // default decoding drops most fillers before they are written down, so an
    // empty result on a non-verbatim transcript says nothing about the take.
    root.Set("verbatim", Value::MakeBool(transcript.verbatim));
    root.Set("words",
             Value::MakeInt(static_cast<int64_t>(transcript.words.size())));
    Value list = Value::MakeArray();
    for (const Disfluency& item : found) {
        Value entry = Value::MakeObject();
        entry.Set(
            "start_word_index",
            Value::MakeInt(static_cast<int64_t>(item.range.start_word_index)));
        entry.Set(
            "end_word_index",
            Value::MakeInt(static_cast<int64_t>(item.range.end_word_index)));
        entry.Set("kind", Value::MakeString(item.kind == DisfluencyKind::Filler
                                                ? "Filler"
                                                : "Repetition"));
        entry.Set("text", Value::MakeString(item.text));
        list.Push(std::move(entry));
    }
    root.Set("disfluencies", std::move(list));
    resultJson = root.Dump();
    return true;
}

bool DispatchRemoveWords(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "ranges"};
    if (!CheckKnownKeys(args, kAllowed, "remove_words", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    if (!ReadId(args, "clip_id", "remove_words", resolver, true, clipId,
                message))
        return Fail(errorName, message, message);
    const Value* ranges = args.Find("ranges");
    if (ranges == nullptr || !ranges->IsArray() || ranges->AsArray().empty())
        return Fail(errorName, message,
                    "'remove_words.ranges' must be a non-empty array");
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!ResolveClipAndTranscript(backend, clipId, document, clip, transcript,
                                  errorName, message))
        return false;

    std::vector<WordRange> wordRanges;
    for (size_t index = 0; index < ranges->AsArray().size(); ++index) {
        const Value& value = ranges->AsArray()[index];
        const std::string path =
            "remove_words.ranges[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(value, {"start_word_index", "end_word_index"}, path,
                            message))
            return Fail(errorName, message, message);
        int64_t start = 0;
        int64_t end = 0;
        if (!ReadInt64(value, "start_word_index", path, true, 0, start,
                       message) ||
            !ReadInt64(value, "end_word_index", path, true, 0, end, message))
            return Fail(errorName, message, message);
        if (start < 0 || end < 0)
            return Fail(errorName, message,
                        path + " word indices must not be negative");
        wordRanges.push_back(
            {static_cast<size_t>(start), static_cast<size_t>(end)});
    }

    RemoveWordsOperation operation;
    if (!ResolveWordRemoval(*clip, transcript, wordRanges, RationalTime{0, 1},
                            {}, operation, message))
        return Fail(errorName, message, message);
    // The clip's A/V partner loses the same frames, in the same operation.
    // Cutting only the clip named would leave picture and sound at different
    // lengths, which is not something a caller asked for by naming words.
    operation.linked_clip_ids =
        LinkedClipIdsFor(document, *clip, operation.ranges);
    return backend.ApplyOperation(operation, resultJson, errorName, message);
}

bool DispatchCreateInterviewShort(McpBackend& backend,
                                  const IdResolver& resolver, const Value& args,
                                  std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    static const std::vector<std::string> kAllowed = {"name", "segments"};
    if (!CheckKnownKeys(args, kAllowed, "create_interview_short", message))
        return Fail(errorName, message, message);
    CreateProjectTimelineFromSegmentsOperation operation;
    if (!ReadString(args, "name", "create_interview_short", false,
                    "Short interview — 60 s", operation.name, message))
        return Fail(errorName, message, message);
    const Value* segments = args.Find("segments");
    if (!segments || !segments->IsArray() || segments->AsArray().empty())
        return Fail(errorName, message,
                    "'create_interview_short.segments' must be a non-empty "
                    "array");
    // Segments are named by span id and resolved here. This is the whole
    // point of the tool's shape: a caller that could pass a source_in could
    // pass one that lands mid-word, and nothing downstream would catch it --
    // ApplyOperation only checks that a range sits inside the media. Taking
    // ids instead makes a cut inside a word unrepresentable rather than
    // merely discouraged (PHILOSOPHY.md principle 7).
    //
    // Arguments are validated before the transcript is read, so a caller
    // still passing the old source_in/duration shape is told which field is
    // wrong instead of being told the transcript is missing -- which it may
    // well also be, and which would send them fixing the wrong thing.
    std::vector<std::pair<std::string, std::string>> requestedSpans;
    for (size_t index = 0; index < segments->AsArray().size(); ++index) {
        const Value& value = segments->AsArray()[index];
        const std::string path =
            "create_interview_short.segments[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(value, {"span_id", "end_span_id"}, path, message))
            return Fail(errorName, message, message);
        std::string startSpanId;
        std::string endSpanId;
        if (!ReadString(value, "span_id", path, true, "", startSpanId,
                        message) ||
            !ReadString(value, "end_span_id", path, false, "", endSpanId,
                        message))
            return Fail(errorName, message, message);
        requestedSpans.emplace_back(std::move(startSpanId),
                                    std::move(endSpanId));
    }
    std::vector<TimelineTranscriptSpan> spans;
    if (!backend.ReadTimelineTranscriptSpans(spans, message)) {
        errorName = "IoError";
        message = "no transcript spans to select from: " + message;
        return false;
    }
    for (size_t index = 0; index < requestedSpans.size(); ++index) {
        ProjectTimelineSourceSegment segment;
        if (!ResolveTranscriptSpanRange(spans, requestedSpans[index].first,
                                        requestedSpans[index].second,
                                        segment.source_id, segment.source_in,
                                        segment.duration, message)) {
            return Fail(errorName, message,
                        "create_interview_short.segments[" +
                            std::to_string(index) + "]: " + message);
        }
        operation.segments.push_back(std::move(segment));
    }
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    operation.width = document.sequence.width;
    operation.height = document.sequence.height;
    operation.frame_rate = document.sequence.frame_rate;
    (void)resolver;
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

bool DispatchCleanDisfluencies(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id",
                                                      "include_repetitions"};
    if (!CheckKnownKeys(args, kAllowed, "clean_disfluencies", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    bool includeRepetitions = false;
    if (!ReadId(args, "clip_id", "clean_disfluencies", resolver, true, clipId,
                message) ||
        !ReadBool(args, "include_repetitions", "clean_disfluencies", false,
                  includeRepetitions, message))
        return Fail(errorName, message, message);
    Document document;
    const DocumentClip* clip = nullptr;
    Transcript transcript;
    if (!ResolveClipAndTranscript(backend, clipId, document, clip, transcript,
                                  errorName, message))
        return false;

    const std::vector<Disfluency> found =
        FindDisfluenciesInClip(*clip, transcript);
    std::vector<WordRange> ranges;
    Value removed = Value::MakeArray();
    for (const Disfluency& item : found) {
        if (item.kind == DisfluencyKind::Repetition && !includeRepetitions)
            continue;
        ranges.push_back(item.range);
        Value entry = Value::MakeObject();
        entry.Set("kind", Value::MakeString(item.kind == DisfluencyKind::Filler
                                                ? "Filler"
                                                : "Repetition"));
        entry.Set("text", Value::MakeString(item.text));
        removed.Push(std::move(entry));
    }

    Value root = Value::MakeObject();
    root.Set("clip_id", Value::MakeString(clip->id));
    root.Set("verbatim", Value::MakeBool(transcript.verbatim));
    root.Set("removed_count",
             Value::MakeInt(static_cast<int64_t>(ranges.size())));
    root.Set("removed", std::move(removed));
    if (ranges.empty()) {
        // Nothing to cut is a success, not a failure: "clean this clip" on an
        // already clean clip has been honoured. Reporting `verbatim` alongside
        // matters here more than anywhere else -- a standard transcript drops
        // most fillers before they are ever written down, so an empty result
        // on one says nothing about the take.
        root.Set("applied", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    RemoveWordsOperation operation;
    if (!ResolveWordRemoval(*clip, transcript, ranges, RationalTime{0, 1}, {},
                            operation, message))
        return Fail(errorName, message, message);
    operation.linked_clip_ids =
        LinkedClipIdsFor(document, *clip, operation.ranges);
    Value linkedView = Value::MakeArray();
    for (const Ulid& linkedId : operation.linked_clip_ids)
        linkedView.Push(Value::MakeString(linkedId));
    root.Set("also_cut", std::move(linkedView));
    std::string operationResult;
    if (!backend.ApplyOperation(operation, operationResult, errorName, message))
        return false;
    root.Set("applied", Value::MakeBool(true));
    // Carry through whatever the apply path reported (notably doc_hash), so
    // a caller can still detect drift after an intent-level tool call.
    Value applied;
    std::string appliedParseError;
    if (Value::Parse(operationResult, applied, appliedParseError) &&
        applied.IsObject()) {
        const Value* hash = applied.Find("doc_hash");
        if (hash != nullptr) root.Set("doc_hash", *hash);
    }
    resultJson = root.Dump();
    return true;
}

bool DispatchListShotQuality(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "list_shot_quality", message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    std::map<Ulid, ShotQualityReport> reports;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (reports.count(clip.source_id)) continue;
            ShotQualityReport report;
            std::string readError;
            if (backend.ReadSourceShotQuality(clip.source_id, report,
                                              readError))
                reports.emplace(clip.source_id, std::move(report));
        }
    }
    resultJson =
        DescribeShotQualityForAgent(document, reports, ShotQualityThresholds{});
    return true;
}

bool DispatchReadFrame(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "media_id",
                                                      "source_in", "position"};
    if (!CheckKnownKeys(args, kAllowed, "read_frame", message))
        return Fail(errorName, message, message);
    const bool hasClip = args.Find("clip_id") != nullptr;
    const bool hasMedia = args.Find("media_id") != nullptr;
    if (hasClip == hasMedia)
        return Fail(errorName, message,
                    "'read_frame' takes exactly one of 'clip_id' or "
                    "'media_id'");

    Ulid sourceId;
    RationalTime time;
    if (hasClip) {
        Ulid clipId;
        std::string position;
        if (!ReadId(args, "clip_id", "read_frame", resolver, true, clipId,
                    message) ||
            !ReadString(args, "position", "read_frame", false, "Middle",
                        position, message))
            return Fail(errorName, message, message);
        if (args.Find("source_in") != nullptr)
            return Fail(errorName, message,
                        "'read_frame.source_in' belongs with 'media_id'; a "
                        "clip is addressed by 'position' instead");
        Document document;
        if (!backend.SnapshotDocument(document, message))
            return Fail(errorName, message, message);
        const DocumentClip* clip = document.FindClip(clipId);
        if (clip == nullptr) {
            errorName = EditErrorName(EditError::UnknownClip);
            message = "unknown clip_id '" + clipId + "'";
            return false;
        }
        sourceId = clip->source_id;
        // Named positions, not a time: the caller says which part of the shot
        // it wants to see and the engine resolves the frame, the same trade
        // create_interview_short makes. "End" steps back one sample interval
        // so it lands inside the clip rather than on the cut after it.
        if (position == "Start") {
            time = clip->source_in;
        } else if (position == "Middle") {
            time = clip->source_in.add(
                RationalTime{clip->duration.value / 2, clip->duration.rate});
        } else if (position == "End") {
            const RationalTime back{clip->duration.rate / 4 + 1,
                                    clip->duration.rate};
            const RationalTime end = clip->source_in.add(clip->duration);
            time = end.compare(back) > 0 ? end.sub(back) : clip->source_in;
        } else {
            return Fail(errorName, message,
                        "'read_frame.position' must be 'Start', 'Middle' or "
                        "'End'");
        }
    } else {
        if (!ReadId(args, "media_id", "read_frame", resolver, true, sourceId,
                    message) ||
            !ReadTime(args, "source_in", "read_frame", true, time, message))
            return Fail(errorName, message, message);
    }

    std::string jpeg;
    if (!backend.CaptureSourceFrame(sourceId, time, jpeg, message)) {
        errorName = "IoError";
        return false;
    }
    Value described = Value::MakeObject();
    described.Set("source_id", Value::MakeString(sourceId));
    Value at = Value::MakeObject();
    at.Set("value", Value::MakeInt(time.value));
    at.Set("rate", Value::MakeInt(time.rate));
    described.Set("source_in", std::move(at));
    described.Set("bytes", Value::MakeInt(static_cast<int64_t>(jpeg.size())));

    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(described.Dump()));
    resultJson = envelope.Dump();
    return true;
}

bool DispatchAnalyzeShotQuality(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id"};
    if (!CheckKnownKeys(args, kAllowed, "analyze_shot_quality", message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    if (!ReadId(args, "media_id", "analyze_shot_quality", resolver, true,
                mediaId, message))
        return Fail(errorName, message, message);
    return backend.AnalyzeSourceShotQuality(mediaId, resultJson, message)
               ? true
               : Fail(errorName, message, message);
}

bool DispatchSetActiveTimeline(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"timeline_id"};
    if (!CheckKnownKeys(args, kAllowed, "set_active_timeline", message))
        return Fail(errorName, message, message);
    SetActiveProjectTimelineOperation operation;
    std::string requested;
    if (!ReadString(args, "timeline_id", "set_active_timeline", true, "",
                    requested, message))
        return Fail(errorName, message, message);
    // Deliberately not run through IdResolver: it resolves ids present on the
    // *active* timeline, and the whole point of this tool is to name one that
    // is not. The engine validates the id against the project instead.
    operation.timeline_id = requested;
    (void)resolver;
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

bool DispatchUndo(McpBackend& backend, const IdResolver&, const Value& args,
                  std::string& resultJson, std::string& errorName,
                  std::string& message) {
    if (!CheckKnownKeys(args, {}, "undo", message))
        return Fail(errorName, message, message);
    return backend.Undo(resultJson, errorName, message);
}

bool DispatchRedo(McpBackend& backend, const IdResolver&, const Value& args,
                  std::string& resultJson, std::string& errorName,
                  std::string& message) {
    if (!CheckKnownKeys(args, {}, "redo", message))
        return Fail(errorName, message, message);
    return backend.Redo(resultJson, errorName, message);
}

}  // namespace

McpToolRegistry::McpToolRegistry() {
    const auto add = [&](std::string name, std::string description,
                         std::string schema, McpDispatchFn fn) {
        tools_.push_back(
            {std::move(name), std::move(description), std::move(schema)});
        dispatch_.push_back(std::move(fn));
    };

    add("insert_clip",
        "Insert a clip from a source into a track at an exact timeline "
        "position, rippling later clips on that track forward.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Destination track."), true)
            .Field("source_id", IdSchema("Source media to read from."), true)
            .Field("source_in", kTimeSchemaText, true)
            .Field("duration", kTimeSchemaText, true)
            .Field("timeline_in", kTimeSchemaText, true)
            .Build("insert_clip arguments"),
        DispatchInsertClip);

    add("remove_clip",
        "Remove a clip and ripple later clips on its track backward to "
        "close the gap.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to remove."), true)
            .Build("remove_clip arguments"),
        DispatchRemoveClip);

    add("clear_clip",
        "Remove a clip in place, leaving a gap instead of rippling later "
        "clips.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to clear."), true)
            .Build("clear_clip arguments"),
        DispatchClearClip);

    add("clear_clips",
        "Remove an arbitrary selection of clips atomically in place, leaving "
        "gaps instead of rippling later clips.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Clips to clear."), true)
            .Build("clear_clips arguments"),
        DispatchClearClips);

    add("trim_clip",
        "Trim one edge of a clip by an exact delta, changing only that "
        "clip's own extent.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to trim."), true)
            .Field("edge", EnumSchema({"Head", "Tail"}, "Which edge to trim."),
                   true)
            .Field("delta", kTimeSchemaText, true)
            .Build("trim_clip arguments"),
        DispatchTrimClip);

    add("move_clip",
        "Move a clip to a new track and/or timeline position, overwriting "
        "anything it lands on top of.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to move."), true)
            .Field("track_id", IdSchema("Destination track."), true)
            .Field("timeline_in", kTimeSchemaText, true)
            .Build("move_clip arguments"),
        DispatchMoveClip);

    add("move_clips",
        "Move an arbitrary selection of clips atomically, preserving every "
        "clip ID and using one undo step.",
        SchemaBuilder()
            .Field("moves",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
                           IdSchema("") + ",\"track_id\":" + IdSchema("") +
                           ",\"timeline_in\":" + std::string(kTimeSchemaText) +
                           "},\"required\":[\"clip_id\",\"track_id\","
                           "\"timeline_in\"],\"additionalProperties\":false}",
                       "One exact destination per selected clip (at least "
                       "two)."),
                   true)
            .Build("move_clips arguments"),
        DispatchMoveClips);

    add("split_clip",
        "Split one clip into two at an exact timeline position strictly "
        "inside it.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to split."), true)
            .Field("timeline_position", kTimeSchemaText, true)
            .Build("split_clip arguments"),
        DispatchSplitClip);

    add("split_at_interval",
        "Cut one clip repeatedly at a regular spacing -- every N seconds -- "
        "in a single call. Give the spacing as a duration (interval "
        "{value:3,rate:1} is every three seconds); the engine converts it "
        "into exact frame positions in the sequence's own timebase, so never "
        "compute frame numbers yourself. Cuts the clip's A/V-linked partners "
        "at the same positions, and undoes in one step. Prefer this over "
        "repeated split_clip calls: it is one round trip instead of one per "
        "cut.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to subdivide."), true)
            .Field("interval", kTimeSchemaText, true)
            .Build("split_at_interval arguments"),
        DispatchSplitAtInterval);

    add("delete_gap",
        "Remove a gap of exact duration starting at an exact position on a "
        "track, rippling everything after it backward. Optionally applies "
        "the same shift to other tracks.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track containing the gap."), true)
            .Field("gap_start", kTimeSchemaText, true)
            .Field("gap_duration", kTimeSchemaText, true)
            .Field(
                "linked_track_ids",
                IdArraySchema("Other tracks to ripple by the same duration."),
                false)
            .Build("delete_gap arguments"),
        DispatchDeleteGap);

    add("detach_audio",
        "Split a video clip's audio into its own clip on an audio track, "
        "linked back to the video clip in sync.",
        SchemaBuilder()
            .Field("video_clip_id",
                   IdSchema("Video clip to detach audio from."), true)
            .Field("audio_track_id",
                   IdSchema("Destination audio track for the detached clip."),
                   true)
            .Build("detach_audio arguments"),
        DispatchDetachAudio);

    add("move_linked_clips",
        "Move every member of one A/V-linked group in a single atomic "
        "gesture, one destination per member.",
        SchemaBuilder()
            .Field("link_group_id",
                   IdSchema("Shared link group of the clips being moved."),
                   true)
            .Field("moves",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
                           IdSchema("") + ",\"track_id\":" + IdSchema("") +
                           ",\"timeline_in\":" + std::string(kTimeSchemaText) +
                           "},\"required\":[\"clip_id\",\"track_id\","
                           "\"timeline_in\"],\"additionalProperties\":false}",
                       "One destination per linked member (at least two)."),
                   true)
            .Build("move_linked_clips arguments"),
        DispatchMoveLinkedClips);

    add("trim_linked_clips",
        "Trim every member of one A/V-linked group in a single atomic "
        "gesture, one edge/delta per member.",
        SchemaBuilder()
            .Field("link_group_id",
                   IdSchema("Shared link group of the clips being trimmed."),
                   true)
            .Field("trims",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
                           IdSchema("") +
                           ",\"edge\":" + EnumSchema({"Head", "Tail"}, "") +
                           ",\"delta\":" + std::string(kTimeSchemaText) +
                           "},\"required\":[\"clip_id\",\"edge\",\"delta\"],"
                           "\"additionalProperties\":false}",
                       "One edge/delta per linked member (at least two)."),
                   true)
            .Build("trim_linked_clips arguments"),
        DispatchTrimLinkedClips);

    add("shorten_linked_clip",
        "Shorten one A/V-linked plan by an editing amount. Prefer this over "
        "trim_linked_clips when the user says to remove N frames or seconds: "
        "CUTMACHINE resolves every linked member and computes the edge sign "
        "and exact RationalTime deterministically. Set preview=true to "
        "validate and return the resolved operation without changing the "
        "project.",
        SchemaBuilder()
            .Field("clip_id",
                   IdSchema("Any video or audio member of the linked plan."),
                   true)
            .Field("edge", EnumSchema({"Head", "Tail"}, "Edge to shorten."),
                   true)
            .Field("amount", PositiveIntSchema("Positive editing amount."),
                   true)
            .Field("unit", EnumSchema({"Frames", "Seconds"}, "Amount unit."),
                   true)
            .Field("preview",
                   BoolSchema("Validate without modifying the project."), false)
            .Build("shorten_linked_clip arguments"),
        DispatchShortenLinkedClip);

    add("ripple_trim",
        "Trim one clip's edge and shift every downstream clip on its track "
        "(and optionally other synced tracks) by the same amount.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip whose edge is trimmed."), true)
            .Field("edge", EnumSchema({"Head", "Tail"}, "Which edge to trim."),
                   true)
            .Field("delta", kTimeSchemaText, true)
            .Field("linked_clip_ids",
                   IdArraySchema("Other clips in the same link group to trim "
                                 "together with clip_id."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema("Additional tracks to ripple even though "
                                 "they hold no member of the trim."),
                   false)
            .Build("ripple_trim arguments"),
        DispatchRippleTrim);

    add("roll_edit",
        "Slide a shared cut between contiguous clip pairs without changing "
        "total sequence duration.",
        SchemaBuilder()
            .Field(
                "pairs",
                ArraySchema(
                    "{\"type\":\"object\",\"properties\":{\"left_clip_id\":" +
                        IdSchema("") + ",\"right_clip_id\":" + IdSchema("") +
                        "},\"required\":[\"left_clip_id\","
                        "\"right_clip_id\"],\"additionalProperties\":"
                        "false}",
                    "Contiguous clip pairs sharing the rolled cut."),
                true)
            .Field("delta", kTimeSchemaText, true)
            .Build("roll_edit arguments"),
        DispatchRollEdit);

    add("slip_edit",
        "Slide the source window under one clip (or several linked clips) "
        "without moving its timeline position or duration.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema(
                       "Clip(s) to slip; more than one requires they share "
                       "a link group."),
                   true)
            .Field("delta", kTimeSchemaText, true)
            .Build("slip_edit arguments"),
        DispatchSlipEdit);

    add("remove_linked_clips",
        "Remove every member of one A/V-linked group (at least two), "
        "rippling each affected track.",
        SchemaBuilder()
            .Field("link_group_id", IdSchema("Shared link group."), true)
            .Field("clip_ids",
                   IdArraySchema("Every member of the link group to remove."),
                   true)
            .Build("remove_linked_clips arguments"),
        DispatchRemoveLinkedClips);

    add("clear_linked_clips",
        "Remove every member of one A/V-linked group (at least two) in "
        "place, leaving gaps instead of rippling.",
        SchemaBuilder()
            .Field("link_group_id", IdSchema("Shared link group."), true)
            .Field("clip_ids",
                   IdArraySchema("Every member of the link group to clear."),
                   true)
            .Build("clear_linked_clips arguments"),
        DispatchClearLinkedClips);

    add("split_linked_clips",
        "Split every member of one A/V-linked group at the same timeline "
        "position in one atomic cut, producing two new link groups.",
        SchemaBuilder()
            .Field("link_group_id", IdSchema("Shared link group."), true)
            .Field("clip_ids",
                   IdArraySchema("Every member of the link group to split."),
                   true)
            .Field("timeline_position", kTimeSchemaText, true)
            .Build("split_linked_clips arguments"),
        DispatchSplitLinkedClips);

    add("paste_clips",
        "Paste a batch of clips (optionally A/V-linked to each other) onto "
        "one or more tracks at exact positions, either inserted or "
        "overwriting what is underneath.",
        SchemaBuilder()
            .Field(
                "clips",
                ArraySchema(
                    "{\"type\":\"object\",\"properties\":{\"label\":" +
                        StringSchema(
                            "Caller-chosen token unique within this call, "
                            "used only to correlate 'link_group' members.") +
                        ",\"track_id\":" + IdSchema("") +
                        ",\"source_id\":" + IdSchema("") +
                        ",\"source_in\":" + std::string(kTimeSchemaText) +
                        ",\"duration\":" + std::string(kTimeSchemaText) +
                        ",\"timeline_in\":" + std::string(kTimeSchemaText) +
                        ",\"link_group\":" +
                        StringSchema(
                            "Optional token shared by clips in this call "
                            "that should become one new A/V link group.") +
                        ",\"anchor\":" +
                        BoolSchema("Marks this item as its link_group's sync "
                                   "anchor; defaults to the first member.") +
                        "},\"required\":[\"label\",\"track_id\",\"source_id\","
                        "\"source_in\",\"duration\",\"timeline_in\"],"
                        "\"additionalProperties\":false}",
                    "Clips to paste."),
                true)
            .Field("overwrite",
                   BoolSchema("Overwrite existing clips under the pasted "
                              "range instead of requiring empty space."),
                   false)
            .Build("paste_clips arguments"),
        DispatchPasteClips);

    add("add_track",
        "Append a new, empty video, audio or caption track at an unused "
        "index.",
        SchemaBuilder()
            .Field("kind",
                   EnumSchema({"video", "audio", "caption"}, "Track kind."),
                   true)
            .Field("index", IntSchema("Unique, non-negative track index."),
                   true)
            .Field("locked", BoolSchema("Start the track locked."), false)
            .Field("sync_lock",
                   BoolSchema("Whether ripple edits on other tracks affect "
                              "this one (default true)."),
                   false)
            .Field("visible", BoolSchema("Initial video visibility."), false)
            .Field("muted", BoolSchema("Initial audio mute state."), false)
            .Field("solo", BoolSchema("Initial audio solo state."), false)
            .Build("add_track arguments"),
        DispatchAddTrack);

    add("import_srt",
        "Import a SubRip file as one exact, reversible caption track.",
        SchemaBuilder()
            .Field("path", StringSchema("Path to the UTF-8 .srt file."), true)
            .Field(
                "index",
                IntSchema("Optional unique track index; appends by default."),
                false)
            .Build("import_srt arguments"),
        DispatchImportSrt);

    add("remove_track", "Remove a track and every clip on it.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to remove."), true)
            .Build("remove_track arguments"),
        DispatchRemoveTrack);

    add("set_track_lock",
        "Lock or unlock a track; locked tracks stay readable but reject "
        "edits.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to lock/unlock."), true)
            .Field("locked", BoolSchema("Desired lock state."), false)
            .Build("set_track_lock arguments"),
        DispatchSetTrackLock);

    add("set_track_sync_lock",
        "Enable or disable whether ripple edits started on other tracks "
        "shift this one.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to change."), true)
            .Field("sync_lock", BoolSchema("Desired sync-lock state."), false)
            .Build("set_track_sync_lock arguments"),
        DispatchSetTrackSyncLock);

    add("set_track_output",
        "Set video visibility or audio mute/solo output state.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to change."), true)
            .Field("visible", BoolSchema("Video track visibility."), true)
            .Field("muted", BoolSchema("Audio track mute state."), true)
            .Field("solo", BoolSchema("Audio track solo state."), true)
            .Build("set_track_output arguments"),
        DispatchSetTrackOutput);

    add("update_sequence",
        "Rename the sequence and/or change its frame size and rate.",
        SchemaBuilder()
            .Field("name", StringSchema("New sequence name."), true)
            .Field("width", IntSchema("Frame width in pixels."), true)
            .Field("height", IntSchema("Frame height in pixels."), true)
            .Field("frame_rate", kFractionSchemaText, true)
            .Build("update_sequence arguments"),
        DispatchUpdateSequence);

    add("set_color_management",
        "Configure the pinned OpenColorIO input, working, and output pipeline.",
        SchemaBuilder()
            .Field("enabled", BoolSchema("Enable project color management."),
                   false)
            .Field("input_gamut",
                   EnumSchema({"rec709", "sony_sgamut3_cine", "sony_sgamut3",
                               "rec2020"},
                              "Source RGB primaries."),
                   true)
            .Field("input_transfer",
                   EnumSchema({"rec709", "sony_slog3", "linear"},
                              "Source transfer function."),
                   true)
            .Field("input_ycbcr_matrix",
                   EnumSchema({"auto", "bt709", "bt2020_ncl"},
                              "YUV decoding matrix."),
                   true)
            .Field(
                "input_range",
                EnumSchema({"auto", "full", "limited"}, "Input signal range."),
                true)
            .Field("working_gamut",
                   EnumSchema({"acescct", "rec2020", "rec709"},
                              "Creative grading working space."),
                   true)
            .Field("output_gamut",
                   EnumSchema({"rec709", "rec2020"}, "Delivery RGB primaries."),
                   true)
            .Field("output_transfer",
                   EnumSchema({"rec709", "hlg"}, "Delivery transfer."), true)
            .Build("set_color_management arguments"),
        DispatchSetColorManagement);

    add("add_bin",
        "Create a new media bin, optionally nested under an existing one.",
        SchemaBuilder()
            .Field("name", StringSchema("Bin name (1-128 bytes)."), true)
            .Field("parent_id",
                   IdSchema("Parent bin; omit or empty for the project root."),
                   false)
            .Build("add_bin arguments"),
        DispatchAddBin);

    add("remove_bin", "Remove an empty bin (no media, no child bins).",
        SchemaBuilder()
            .Field("bin_id", IdSchema("Bin to remove."), true)
            .Build("remove_bin arguments"),
        DispatchRemoveBin);

    add("rename_bin", "Rename an existing bin.",
        SchemaBuilder()
            .Field("bin_id", IdSchema("Bin to rename."), true)
            .Field("name", StringSchema("New bin name (1-128 bytes)."), true)
            .Build("rename_bin arguments"),
        DispatchRenameBin);

    add("move_bin",
        "Move a bin under a different parent (or to the project root).",
        SchemaBuilder()
            .Field("bin_id", IdSchema("Bin to move."), true)
            .Field("parent_id",
                   IdSchema("New parent bin; omit or empty for the project "
                            "root."),
                   false)
            .Build("move_bin arguments"),
        DispatchMoveBin);

    add("set_media_bin",
        "File a library media item into a bin (or back to the project "
        "root).",
        SchemaBuilder()
            .Field("media_id", IdSchema("Library media item."), true)
            .Field("bin_id",
                   IdSchema("Destination bin; omit or empty for the project "
                            "root."),
                   false)
            .Build("set_media_bin arguments"),
        DispatchSetMediaBin);

    add("add_marker", "Add a sequence marker at an exact time.",
        SchemaBuilder()
            .Field("name", StringSchema("Marker label."), true)
            .Field("time", kTimeSchemaText, true)
            .Field("color", StringSchema("Marker color, e.g. '#f5c542'."),
                   false)
            .Field("category", StringSchema("Marker category."), false)
            .Field("insertion_index",
                   IntSchema("Position in the marker list; omit to append."),
                   false)
            .Build("add_marker arguments"),
        DispatchAddMarker);

    add("remove_marker", "Remove a sequence marker.",
        SchemaBuilder()
            .Field("marker_id", IdSchema("Marker to remove."), true)
            .Build("remove_marker arguments"),
        DispatchRemoveMarker);

    add("update_marker", "Replace a marker's name, time, color and category.",
        SchemaBuilder()
            .Field("marker_id", IdSchema("Marker to update."), true)
            .Field("name", StringSchema("New marker label."), true)
            .Field("time", kTimeSchemaText, true)
            .Field("color", StringSchema("New marker color."), true)
            .Field("category", StringSchema("New marker category."), true)
            .Build("update_marker arguments"),
        DispatchUpdateMarker);

    add("add_transition",
        "Add a transition spanning the cut between two adjacent clips on a "
        "track.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track the transition belongs to."),
                   true)
            .Field("left_clip_id", IdSchema("Outgoing clip."), true)
            .Field("right_clip_id", IdSchema("Incoming clip."), true)
            .Field("type",
                   StringSchema("Transition type, e.g. 'cross_dissolve'."),
                   false)
            .Field("duration", kTimeSchemaText, true)
            .Field("alignment",
                   EnumSchema({"Center", "StartAtCut", "EndAtCut"},
                              "How the transition straddles the cut."),
                   false)
            .Field(
                "insertion_index",
                IntSchema("Position in the transition list; omit to append."),
                false)
            .Build("add_transition arguments"),
        DispatchAddTransition);

    add("remove_transition", "Remove a transition.",
        SchemaBuilder()
            .Field("transition_id", IdSchema("Transition to remove."), true)
            .Build("remove_transition arguments"),
        DispatchRemoveTransition);

    add("update_transition",
        "Replace a transition's type, duration and alignment.",
        SchemaBuilder()
            .Field("transition_id", IdSchema("Transition to update."), true)
            .Field("type", StringSchema("New transition type."), false)
            .Field("duration", kTimeSchemaText, true)
            .Field("alignment",
                   EnumSchema({"Center", "StartAtCut", "EndAtCut"}, ""), false)
            .Build("update_transition arguments"),
        DispatchUpdateTransition);

    add("set_clip_link",
        "Link two clips as an A/V pair, or clear an existing link between "
        "them.",
        SchemaBuilder()
            .Field("first_clip_id", IdSchema("First clip."), true)
            .Field("second_clip_id", IdSchema("Second clip."), true)
            .Field(
                "linked",
                BoolSchema("true to link (default), false to clear the link."),
                false)
            .Build("set_clip_link arguments"),
        DispatchSetClipLink);

    add("set_clip_effects", "Replace a clip's entire color-grade effect stack.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to grade."), true)
            .Field("effects",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"type\":" +
                           StringSchema(
                               "Dotted knob family, e.g. 'color.exposure'.") +
                           ",\"params\":{\"type\":\"object\",\"description\":"
                           "\"Knob name to exact num/den value.\","
                           "\"additionalProperties\":" +
                           std::string(kFractionSchemaText) +
                           "}},\"required\":[\"type\"],"
                           "\"additionalProperties\":false}",
                       "Ordered effect stack; replaces the clip's current "
                       "stack entirely."),
                   true)
            .Build("set_clip_effects arguments"),
        DispatchSetClipEffects);

    add("set_clip_opacity",
        "Set a video clip's compositing opacity as an exact fraction.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Video clip to update."), true)
            .Field("opacity", kFractionSchemaText, true)
            .Build("set_clip_opacity arguments"),
        DispatchSetClipOpacity);

    add("add_caption_style", "Add a new caption style to the sequence.",
        SchemaBuilder()
            .Field("font_family", StringSchema("Font family name."), false)
            .Field("font_size", IntSchema("Font size in points."), false)
            .Field("color", StringSchema("Text color, e.g. '#ffffff'."), false)
            .Field("position",
                   EnumSchema({"bottom", "top", "center"},
                              "On-screen caption position."),
                   false)
            .Field("insertion_index",
                   IntSchema("Position in the style list; omit to append."),
                   false)
            .Build("add_caption_style arguments"),
        DispatchAddCaptionStyle);

    add("remove_caption_style",
        "Remove a caption style not currently referenced by any clip.",
        SchemaBuilder()
            .Field("style_id", IdSchema("Caption style to remove."), true)
            .Build("remove_caption_style arguments"),
        DispatchRemoveCaptionStyle);

    add("set_clip_caption",
        "Join a clip to a caption run (or clear its caption) and set its "
        "slice of the caption text.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to caption."), true)
            .Field("caption_group_id",
                   IdSchema("Caption style/run to join; omit or empty to "
                            "clear the clip's caption."),
                   false)
            .Field("caption_text",
                   StringSchema("This clip's slice of the caption text."),
                   false)
            .Build("set_clip_caption arguments"),
        DispatchSetClipCaption);

    add("describe",
        "Return the same JSON snapshot of the project as `--describe`: "
        "sequence, tracks, clips, sources, library, bins and markers.",
        SchemaBuilder().Build("describe takes no arguments"), DispatchDescribe);

    add("get_timeline_transcript",
        "Read the cached transcript of audible clips as selectable semantic "
        "spans. Call this before planning an interview edit, then name the "
        "spans you want by their span_id. The exact times are shown so a "
        "human can check them; never retype one into another tool.",
        SchemaBuilder().Build("get_timeline_transcript takes no arguments"),
        DispatchGetTimelineTranscript);

    const std::string spanSegmentSchema =
        "{\"type\":\"object\",\"properties\":{\"span_id\":" +
        StringSchema(
            "First transcript span of this segment, as returned by "
            "get_timeline_transcript (for example \"S12\").") +
        ",\"end_span_id\":" +
        StringSchema(
            "Optional last span of a contiguous run, inclusive. Use "
            "it when one idea covers several spans; CUTMACHINE "
            "merges them into one exact range.") +
        "},\"required\":[\"span_id\"],\"additionalProperties\":false}";
    add("create_interview_short",
        "Create and activate a new timeline from transcript spans, leaving "
        "the original timeline unchanged. Name spans by their span_id from "
        "get_timeline_transcript and order them as hook, concise "
        "development, then payoff. Never compute or copy a timecode: "
        "CUTMACHINE resolves each span id to its exact source range, which "
        "is what keeps a cut off the middle of a word.",
        SchemaBuilder()
            .Field("name", StringSchema("Name of the new timeline."), false)
            .Field("segments",
                   ArraySchema(spanSegmentSchema,
                               "Transcript spans in final editorial order."),
                   true)
            .Build("create_interview_short arguments"),
        DispatchCreateInterviewShort);

    add("clean_disfluencies",
        "Remove every detected hesitation from one clip as a single "
        "reversible operation, without the caller enumerating anything. "
        "Detection is deterministic (a filler-syllable lexicon plus "
        "immediate repetitions) and CUTMACHINE resolves the frames. "
        "Repetitions are left in place unless asked for, because an "
        "immediately repeated word can be deliberate emphasis while a "
        "filler syllable never is. Requires a verbatim transcript: standard "
        "decoding drops most fillers before they are written down, and the "
        "result reports which kind it read.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to clean."), true)
            .Field("include_repetitions",
                   BoolSchema("Also remove immediately repeated words. "
                              "Defaults to false."),
                   false)
            .Build("clean_disfluencies arguments"),
        DispatchCleanDisfluencies);

    add("list_shot_quality",
        "Report measured picture quality for every video clip on the active "
        "timeline: whether each is Sharp/Soft/Blurry and Steady/Moving/"
        "Shaky, with the numbers behind each grade. Measured by CUTMACHINE "
        "from the pictures themselves, never judged by a model. Use it to "
        "keep soft or unsteady shots out of a cut. Clips whose source has "
        "not been analysed are listed under \"unanalyzed\" and are not a "
        "pass: they are unknown.",
        SchemaBuilder().Build("list_shot_quality takes no arguments"),
        DispatchListShotQuality);

    add("list_disfluencies",
        "List the fillers (euh, heu, hum, ben) and stuttered repetitions "
        "found in one clip's cached transcript, as word index ranges ready "
        "for remove_words. Detection is deterministic, not a judgement call: "
        "review the text of each entry, drop the ones that carry meaning, "
        "and pass the rest through. Only a transcript made with verbatim "
        "decoding contains these words at all -- the reply says which kind "
        "it read.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip whose transcript to inspect."),
                   true)
            .Build("list_disfluencies arguments"),
        DispatchListDisfluencies);

    const std::string wordRangeSchema =
        "{\"type\":\"object\",\"properties\":{\"start_word_index\":" +
        IntSchema("First word to remove, inclusive.") +
        ",\"end_word_index\":" + IntSchema("Last word to remove, inclusive.") +
        "},\"required\":[\"start_word_index\",\"end_word_index\"],"
        "\"additionalProperties\":false}";
    add("remove_words",
        "Remove word ranges from one clip and ripple-close every cut, as a "
        "single reversible operation. Give word indices only, taken from "
        "list_disfluencies: CUTMACHINE resolves which frames they are. Never "
        "compute a timecode yourself. Ranges must be sorted and must not "
        "overlap.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to cut."), true)
            .Field("ranges",
                   ArraySchema(wordRangeSchema,
                               "Sorted, non-overlapping word index ranges."),
                   true)
            .Build("remove_words arguments"),
        DispatchRemoveWords);

    add("read_frame",
        "Look at one frame. Returns the picture itself, so you can judge "
        "what no measurement can: whether someone is visibly speaking (a "
        "cutaway of a talking person over other dialogue reads as a sync "
        "error), whether the subject is in frame, what the shot actually "
        "shows. Use it before placing an illustration, and to check a "
        "candidate you have not seen. Give either a clip_id with an optional "
        "position, or a media_id with an exact source_in.",
        SchemaBuilder()
            .Field("clip_id",
                   IdSchema("Clip on the active timeline to look at."), false)
            .Field("position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Which part of the clip to show. Defaults to "
                              "Middle. Only with clip_id."),
                   false)
            .Field("media_id",
                   IdSchema("Library media to look at, for a shot that is "
                            "not on the timeline yet."),
                   false)
            .Field("source_in", kTimeSchemaText, false)
            .Build("read_frame arguments"),
        DispatchReadFrame);

    add("analyze_shot_quality",
        "Measure a source's picture quality and cache the result, so "
        "list_shot_quality has something to report. Decodes the whole media, "
        "so it takes seconds to a minute depending on length; the result is "
        "cached and only needs redoing when the media changes. Run it on any "
        "clip listed under \"unanalyzed\".",
        SchemaBuilder()
            .Field("media_id", IdSchema("Media to analyse."), true)
            .Build("analyze_shot_quality arguments"),
        DispatchAnalyzeShotQuality);

    add("set_active_timeline",
        "Switch which timeline the other tools act on. Every editing tool "
        "addresses the active timeline, so this is how you move between the "
        "timelines a project holds.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full ID of the timeline to activate."), true)
            .Build("set_active_timeline arguments"),
        DispatchSetActiveTimeline);

    add("undo",
        "Undo the most recently applied operation on the active timeline, "
        "restoring the document to its exact prior byte-for-byte state.",
        SchemaBuilder().Build("undo takes no arguments"), DispatchUndo);

    add("redo",
        "Redo the most recently undone operation on the active timeline.",
        SchemaBuilder().Build("redo takes no arguments"), DispatchRedo);
}

namespace {

// Unwraps the reserved envelope (McpTools.h) a dispatcher returns when it
// has a picture, so no caller has to know the convention: the outcome ends
// up with the tool's own result text and the image beside it.
void LiftImagePayload(McpToolCallOutcome& outcome) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(outcome.result_json, parsed, parseError) ||
        !parsed.IsObject()) {
        return;
    }
    const Value* data = parsed.Find(kMcpImageDataKey);
    const Value* mime = parsed.Find(kMcpImageMimeKey);
    if (data == nullptr || !data->IsString() || mime == nullptr ||
        !mime->IsString()) {
        return;
    }
    const Value* text = parsed.Find(kMcpImageTextKey);
    if (text == nullptr || !text->IsString()) return;
    outcome.image_base64 = data->AsString();
    outcome.image_mime = mime->AsString();
    outcome.result_json = text->AsString();
}

}  // namespace

McpToolCallOutcome McpToolRegistry::Call(
    McpBackend& backend, const std::string& toolName,
    const mcp_json::Value& arguments) const {
    McpToolCallOutcome outcome;
    size_t toolIndex = tools_.size();
    for (size_t index = 0; index < tools_.size(); ++index) {
        if (tools_[index].name == toolName) {
            toolIndex = index;
            break;
        }
    }
    if (toolIndex == tools_.size()) {
        outcome.ok = false;
        outcome.error_name = "UnknownTool";
        outcome.message = "no such tool '" + toolName + "'";
        return outcome;
    }

    // MCP allows omitting "arguments" for a no-argument tool; treat Null as
    // an empty object rather than a type error.
    static const Value kEmptyObject = Value::MakeObject();
    const Value& effectiveArguments =
        arguments.IsNull() ? kEmptyObject : arguments;
    if (!effectiveArguments.IsObject()) {
        outcome.ok = false;
        outcome.error_name = "ValidationFailed";
        outcome.message = "'" + toolName + "' arguments must be a JSON object";
        return outcome;
    }

    Document document;
    std::string snapshotError;
    if (!backend.SnapshotDocument(document, snapshotError)) {
        outcome.ok = false;
        outcome.error_name = "IoError";
        outcome.message = snapshotError;
        return outcome;
    }
    const IdResolver resolver(document);

    outcome.ok = dispatch_[toolIndex](backend, resolver, effectiveArguments,
                                      outcome.result_json, outcome.error_name,
                                      outcome.message);
    if (outcome.ok) LiftImagePayload(outcome);
    return outcome;
}

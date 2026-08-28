#pragma once

// The MCP tool catalog (ROADMAP.md F1.2): one entry per operation in
// Operations.h that a human can already trigger through the app or the
// `--apply-op` CLI path, plus `describe`/`undo`/`redo`. Excluded on purpose:
//   - JoinClip: not a human-triggerable gesture anywhere in the app: it is
//     only ever produced as SplitClip's stored inverse, and constructing one
//     by hand requires supplying the exact pre-split clip extents. Its
//     effect is already reachable through the `undo` tool.
//   - AddMulticamGroup / SetMulticamActiveAngle: Operations.cc rejects both
//     with EditError::InvalidOperation pending ROADMAP.md ticket F1.5; a
//     tool that always fails would not be a real capability.
//
// Every dispatch function here validates its JSON arguments (rejecting
// unknown keys and non-finite numbers, naming the offending field),
// resolves short IDs via IdResolver, constructs the matching Operations.h
// struct, and hands it to McpBackend -- never mutates a Document itself.

#include "IdResolver.h"
#include "Json.h"
#include "McpBackend.h"

#include <functional>
#include <string>
#include <vector>

// One tool's JSON-RPC-facing dispatch: given the backend, an ID resolver
// already built over the backend's current document, and the tool's
// "arguments" object, either applies an operation and reports success, or
// fails with errorName/message naming what was wrong. Never throws.
using McpDispatchFn = std::function<bool(
    McpBackend& backend, const IdResolver& resolver,
    const mcp_json::Value& arguments, std::string& resultJson,
    std::string& errorName, std::string& message)>;

// Metadata for one MCP tool, as surfaced by `tools/list`.
struct McpTool {
    std::string name;
    std::string description;
    // Raw JSON text of the tool's JSON Schema `inputSchema` object.
    std::string input_schema_json;
};

struct McpToolCallOutcome {
    bool ok = false;
    std::string result_json;
    std::string error_name;
    std::string message;
    // A tool may return a picture as well as text (today only read_frame).
    // Base64, with its MIME type; empty when the tool returned text only.
    //
    // Carried out of the dispatcher through two reserved keys in its result
    // JSON rather than by widening McpDispatchFn: the signature is shared by
    // every tool in the catalog, and changing it to thread an out-parameter
    // no other tool populates would touch all of them to serve one. Call()
    // lifts the keys into these fields and removes them from the text, so
    // nothing downstream sees the convention.
    std::string image_base64;
    std::string image_mime;
};

// The reserved envelope a dispatcher returns when it has a picture: the two
// image fields plus the result text it would otherwise have returned on its
// own. An envelope rather than extra keys on the real payload, so lifting it
// is a lookup instead of a rebuild -- mcp_json::Value has no key
// enumeration, and widening a shared type to serve one tool would be the
// wrong trade.
inline constexpr char kMcpImageDataKey[] = "__image_base64";
inline constexpr char kMcpImageMimeKey[] = "__image_mime";
inline constexpr char kMcpImageTextKey[] = "__text";

class McpToolRegistry {
public:
    McpToolRegistry();

    const std::vector<McpTool>& Tools() const { return tools_; }

    // Looks up `toolName` and, if found, snapshots the backend's current
    // document to build an IdResolver and runs the tool's dispatch
    // function. Returns an outcome with error_name "UnknownTool" if no such
    // tool is registered.
    McpToolCallOutcome Call(McpBackend& backend, const std::string& toolName,
                            const mcp_json::Value& arguments) const;

private:
    std::vector<McpTool> tools_;
    std::vector<McpDispatchFn> dispatch_;  // index-aligned with tools_
};

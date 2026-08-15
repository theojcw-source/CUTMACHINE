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
};

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

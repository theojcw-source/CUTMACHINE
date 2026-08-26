#pragma once

// Backend seam between the transport (McpServer/HttpServer) and tool
// dispatch (McpTools) on one side, and the actual edit path on the other.
//
// There are two implementations:
//  - McpProjectBackend (McpProjectBackend.h): the production backend. It
//    reuses ApplyOperationCommand/UndoOperationCommand/RedoOperationCommand/
//    DescribeCommand from Cli.cc verbatim -- the exact same load/apply/
//    commit path a `--apply-op` CLI invocation takes. It depends on
//    ProjectStorage, so (like Cli.cc's project-facing commands) it only
//    links where CommonCrypto is available (macOS).
//  - A pure in-memory backend used by tests (tests/mcp_tools_tests.cc),
//    which calls EditLog::Apply/Undo/Redo directly against a Document held
//    in memory. That is still the exact same apply path -- EditLog::Apply
//    is what ApplyOperationCommand calls -- just without the project-file
//    round trip, so it builds and runs without FFmpeg/CommonCrypto/AppKit
//    for local iteration and CI on non-macOS hosts.
//
// Either way, McpTools.cc never mutates a Document itself: every tool
// dispatch function ends by constructing an Operation and handing it to
// this interface. Errors are reported by name (the same strings
// EditErrorName() in Operations.h produces) rather than as an EditError
// enum, so both implementations -- one working from a JSON string returned
// by a Cli.cc command, the other from a live EditError -- report through
// one shape without a reverse enum<->string mapping.

#include "Document.h"
#include "Operations.h"

#include <string>

class McpBackend {
public:
    virtual ~McpBackend() = default;

    // Read-only snapshot of the current document, used only to resolve
    // short IDs (IdResolver) before an operation is constructed.
    virtual bool SnapshotDocument(Document& document, std::string& message) = 0;

    // Applies a freshly constructed (not-yet-applied) operation through the
    // same path as `--apply-op`. On success, resultJson holds the tool
    // result payload to report back over MCP. On failure, errorName is one
    // of the EditErrorName() strings from Operations.h.
    virtual bool ApplyOperation(Operation operation, std::string& resultJson,
                                std::string& errorName,
                                std::string& message) = 0;

    // Project-level edit used by agent-created timelines. Backends that only
    // expose a standalone Document may leave this unsupported.
    virtual bool ApplyProjectEdit(ProjectOperation, std::string&,
                                  std::string& errorName,
                                  std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot create project timelines";
        return false;
    }

    // Read-only cached transcript view. Kept behind the backend because the
    // cache directory belongs to the open project, not the Document JSON.
    virtual bool ReadTimelineTranscript(std::string&, std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    virtual bool Undo(std::string& resultJson, std::string& errorName,
                      std::string& message) = 0;
    virtual bool Redo(std::string& resultJson, std::string& errorName,
                      std::string& message) = 0;

    // Same JSON shape as `--describe`.
    virtual bool Describe(std::string& json, std::string& message) = 0;
};

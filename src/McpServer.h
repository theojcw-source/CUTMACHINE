#pragma once

// MCP server: JSON-RPC 2.0 over HTTP (ROADMAP.md F1.1), implementing the
// Model Context Protocol's `initialize`, `tools/list` and `tools/call`
// methods on top of HttpServer.h. Tool semantics live entirely in
// McpTools.cc/McpBackend.h; this file only owns the JSON-RPC envelope:
// request parsing, method dispatch, response/notification/error shaping.
//
// Deliberate scope limits versus the full MCP "Streamable HTTP" transport:
//   - Every response is a single `application/json` object (never an SSE
//     stream). The spec allows this for transports that never need to
//     stream additional server-initiated messages, which is true here: this
//     server never sends unsolicited notifications.
//   - A JSON-RPC request with no "id" (a notification, e.g.
//     `notifications/initialized`) is accepted and acknowledged with an
//     empty HTTP 202, matching "notifications get no JSON-RPC response".
//   - Tool execution failures (unknown clip, overlap, unknown tool name,
//     bad arguments, ...) are reported as a `tools/call` result with
//     `isError:true`, never as a JSON-RPC protocol-level error -- per spec,
//     that is what lets the calling agent see and react to the failure.
//     JSON-RPC error responses are reserved for envelope-level problems:
//     malformed JSON, an unknown method, or malformed `tools/call` params.
//
// Every `tools/call` that reaches this server is executed with the full
// authority of the user's project -- there is no authentication, consent
// prompt or audit gate in front of it. That is only tenable because
// HttpServer.h binds loopback and nothing else. Read the warning at the top
// of HttpServer.h before changing how this server is reached.

#include "HttpServer.h"
#include "McpBackend.h"
#include "McpTools.h"

#include <string>

class McpServer {
public:
    // Does not take ownership of `backend`; it must outlive the server.
    explicit McpServer(McpBackend& backend);

    // Starts listening on 127.0.0.1:port (0 asks the OS for a free port --
    // see Port()). The single path handled is `path` (default "/mcp");
    // everything else gets HTTP 404.
    bool Start(int port, std::string& error, const std::string& path = "/mcp");
    void Stop();
    bool IsRunning() const { return http_.IsRunning(); }
    int Port() const { return http_.Port(); }

    // Exposed for tests that want to exercise the JSON-RPC envelope
    // directly without going through a real HTTP round trip. Returns true
    // and fills `responseBody` for an ordinary request; returns false (no
    // body) for a notification, mirroring HTTP 202-with-no-body.
    bool HandleJsonRpc(const std::string& requestBody,
                       std::string& responseBody);

private:
    McpBackend& backend_;
    McpToolRegistry tools_;
    HttpServer http_;
    std::string path_;
};

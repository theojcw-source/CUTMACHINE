#include "McpServer.h"

#include "Json.h"

namespace {

using mcp_json::Value;

std::string Esc(const std::string& text) {
    return mcp_json::EscapeJsonString(text);
}

std::string JsonRpcResult(const Value& id, const std::string& resultJson) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id.Dump() +
           ",\"result\":" + resultJson + "}";
}

std::string JsonRpcError(const Value& id, int code, const std::string& text) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id.Dump() +
           ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" +
           Esc(text) + "\"}}";
}

// DELTA-2026-08 -- a delta nobody is told about is dead weight. This is the
// one place the protocol lets the server explain how to use its own results,
// and it is what stops a caller re-reading the whole timeline after every
// edit the way one did before the delta existed.
const char* kServerInstructions =
    "Read the timeline once with `describe`, then keep your own model of it. "
    "Every mutating tool returns a `delta` describing exactly what changed, "
    "so patch that model instead of calling `describe` again -- a full "
    "snapshot costs hundreds of kilobytes and cannot tell a change you caused "
    "from one you did not.\n\n"
    "Delta vocabulary: `clips` is the resulting state of every clip created "
    "or changed, each with its track and exact times (`created` marks one "
    "that did not exist before); `shifted` is a ripple stated as a rule -- on "
    "`track_id`, every clip at or after `from` moved by `by`, `count` of "
    "them -- and is only emitted when it explains that whole stretch, so it "
    "can be applied literally; `removed_clip_ids`, `created_tracks` and "
    "`removed_track_ids` say what appeared and disappeared; `duration` is the "
    "timeline's new length. Empty sections are omitted, and "
    "`sequence_changed` means re-read the sequence header only. `undo` and "
    "`redo` return a delta too, which is the only way to learn what a "
    "reversal put back.\n\n"
    "Re-read with `describe` only after a failure that suggests your model is "
    "stale, or when something changed outside this session.";

std::string InitializeResultJson(const std::string& protocolVersion) {
    return "{\"protocolVersion\":\"" + Esc(protocolVersion) +
           "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":"
           "\"cutmachine\",\"version\":\"0.1.0\"},\"instructions\":\"" +
           Esc(kServerInstructions) + "\"}";
}

std::string ToolsListResultJson(const McpToolRegistry& registry) {
    std::string tools = "[";
    bool first = true;
    for (const McpTool& tool : registry.Tools()) {
        if (!first) tools += ",";
        first = false;
        tools += "{\"name\":\"" + Esc(tool.name) + "\",\"description\":\"" +
                 Esc(tool.description) +
                 "\",\"inputSchema\":" + tool.input_schema_json + "}";
    }
    tools += "]";
    return "{\"tools\":" + tools + "}";
}

std::string ToolCallResultJson(const McpToolCallOutcome& outcome) {
    std::string content = "{\"type\":\"text\",\"text\":\"" +
                          Esc(outcome.result_json) + "\"}";
    // A tool that returned a picture contributes a second content block, in
    // the shape the MCP specification gives image results. The text block
    // stays first so a client that ignores images still gets the answer.
    if (!outcome.image_base64.empty() && !outcome.image_mime.empty()) {
        content += ",{\"type\":\"image\",\"data\":\"" +
                   Esc(outcome.image_base64) + "\",\"mimeType\":\"" +
                   Esc(outcome.image_mime) + "\"}";
    }
    return "{\"content\":[" + content +
           "],\"isError\":" + (outcome.ok ? "false" : "true") + "}";
}

}  // namespace

McpServer::McpServer(McpBackend& backend) : backend_(backend) {}

bool McpServer::Start(int port, std::string& error, const std::string& path) {
    path_ = path;
    return http_.Start(
        port,
        [this](const std::string& method, const std::string& requestPath,
               const std::string& body, int& statusCode,
               std::string& contentType, std::string& responseBody) {
            contentType = "application/json";
            if (requestPath != path_) {
                statusCode = 404;
                responseBody = "{\"error\":\"not found\"}";
                return;
            }
            if (method != "POST") {
                statusCode = 405;
                responseBody = "{\"error\":\"method not allowed, use POST\"}";
                return;
            }
            std::string jsonRpcResponse;
            if (HandleJsonRpc(body, jsonRpcResponse)) {
                statusCode = 200;
                responseBody = std::move(jsonRpcResponse);
            } else {
                // JSON-RPC notification: acknowledged, no body.
                statusCode = 202;
                responseBody.clear();
            }
        },
        error);
}

void McpServer::Stop() { http_.Stop(); }

bool McpServer::HandleJsonRpc(const std::string& requestBody,
                              std::string& responseBody) {
    Value request;
    std::string parseError;
    if (!Value::Parse(requestBody, request, parseError)) {
        responseBody = JsonRpcError(Value::MakeNull(), -32700,
                                    "Parse error: " + parseError);
        return true;
    }
    if (!request.IsObject()) {
        responseBody = JsonRpcError(Value::MakeNull(), -32600,
                                    "Invalid Request: expected a JSON object");
        return true;
    }

    const Value* idField = request.Find("id");
    const bool isNotification = (idField == nullptr);
    const Value id = idField ? *idField : Value::MakeNull();

    const Value* jsonrpcField = request.Find("jsonrpc");
    const Value* methodField = request.Find("method");
    if (!jsonrpcField || !jsonrpcField->IsString() ||
        jsonrpcField->AsString() != "2.0" || !methodField ||
        !methodField->IsString()) {
        if (isNotification) return false;
        responseBody = JsonRpcError(
            id, -32600,
            "Invalid Request: 'jsonrpc' must be \"2.0\" and 'method' a string");
        return true;
    }
    if (isNotification) {
        // No server-initiated behavior depends on any notification MCP
        // clients send today (e.g. notifications/initialized); acknowledge
        // and move on.
        return false;
    }

    const std::string method = methodField->AsString();
    static const Value kEmptyObject = Value::MakeObject();
    const Value* paramsField = request.Find("params");
    const Value& params = paramsField ? *paramsField : kEmptyObject;

    if (method == "initialize") {
        std::string protocolVersion = "2024-11-05";
        const Value* requested = params.Find("protocolVersion");
        if (requested && requested->IsString())
            protocolVersion = requested->AsString();
        responseBody = JsonRpcResult(id, InitializeResultJson(protocolVersion));
        return true;
    }
    if (method == "tools/list") {
        responseBody = JsonRpcResult(id, ToolsListResultJson(tools_));
        return true;
    }
    if (method == "tools/call") {
        if (!params.IsObject()) {
            responseBody =
                JsonRpcError(id, -32602, "Invalid params: expected an object");
            return true;
        }
        const Value* nameField = params.Find("name");
        if (!nameField || !nameField->IsString()) {
            responseBody =
                JsonRpcError(id, -32602, "Invalid params: 'name' is required");
            return true;
        }
        const Value* argumentsField = params.Find("arguments");
        const Value arguments =
            argumentsField ? *argumentsField : Value::MakeObject();
        const McpToolCallOutcome outcome =
            tools_.Call(backend_, nameField->AsString(), arguments);
        responseBody = JsonRpcResult(id, ToolCallResultJson(outcome));
        return true;
    }

    responseBody = JsonRpcError(id, -32601, "Method not found: " + method);
    return true;
}

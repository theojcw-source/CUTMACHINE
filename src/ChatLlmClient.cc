#include "ChatLlmClient.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace chat {

namespace {

using mcp_json::Value;

std::string Esc(const std::string& text) {
    return mcp_json::EscapeJsonString(text);
}

// ---------------------------------------------------------------------
// Local dotenv loading -- mirrors sidecar/planner.py's _load_project_env:
// blank lines and '#' comments are skipped, an optional leading "export "
// is stripped, the first '=' splits key from value, and a value wrapped in
// matching quotes has them stripped. Every key is set with overwrite=0, so
// a variable already exported in the real environment always wins.
// ---------------------------------------------------------------------

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

void LoadDotEnvFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) return;
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed.rfind("export ", 0) == 0) trimmed = Trim(trimmed.substr(7));
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, equals));
        std::string value = Trim(trimmed.substr(equals + 1));
        if (key.empty()) continue;
        if (value.size() >= 2 && value.front() == value.back() &&
            (value.front() == '\'' || value.front() == '"')) {
            value = value.substr(1, value.size() - 2);
        }
        ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

// $CUTMACHINE_ENV_FILE if set, else ~/.config/cutmachine/.env -- see
// ChatLlmConfig::FromEnvironment's doc comment for why this is not the
// sidecar's project-root .env.
void LoadLocalEnvFileIfPresent() {
    if (const char* explicitPath = std::getenv("CUTMACHINE_ENV_FILE");
        explicitPath && *explicitPath) {
        LoadDotEnvFile(explicitPath);
        return;
    }
    const char* home = std::getenv("HOME");
    if (home && *home)
        LoadDotEnvFile(std::string(home) + "/.config/cutmachine/.env");
}

std::string EnvString(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// ---------------------------------------------------------------------
// Request body assembly. Hand-built JSON text rather than mcp_json::Value
// trees, matching this project's existing convention at the same kind of
// boundary (see McpServer.cc's JsonRpcResult/ToolsListResultJson) --
// McpTool::input_schema_json in particular is already-serialized JSON text
// that only needs splicing in, not a parse/rebuild round trip.
// ---------------------------------------------------------------------

std::string DumpContentBlock(const ChatContentBlock& block) {
    switch (block.type) {
        case ChatBlockType::Text:
            return "{\"type\":\"text\",\"text\":\"" + Esc(block.text) + "\"}";
        case ChatBlockType::ToolUse:
            return "{\"type\":\"tool_use\",\"id\":\"" + Esc(block.tool_use_id) +
                   "\",\"name\":\"" + Esc(block.tool_name) +
                   "\",\"input\":" + block.tool_input.Dump() + "}";
        case ChatBlockType::ToolResult:
            return "{\"type\":\"tool_result\",\"tool_use_id\":\"" +
                   Esc(block.tool_use_id) + "\",\"content\":\"" +
                   Esc(block.text) + "\",\"is_error\":" +
                   (block.tool_is_error ? "true" : "false") + "}";
    }
    return "{}";
}

std::string DumpMessages(const std::vector<ChatMessage>& messages) {
    std::string json = "[";
    for (size_t index = 0; index < messages.size(); ++index) {
        if (index) json += ",";
        const ChatMessage& message = messages[index];
        json += "{\"role\":\"" + Esc(message.role) + "\",\"content\":[";
        for (size_t blockIndex = 0; blockIndex < message.content.size();
             ++blockIndex) {
            if (blockIndex) json += ",";
            json += DumpContentBlock(message.content[blockIndex]);
        }
        json += "]}";
    }
    json += "]";
    return json;
}

std::string DumpTools(const std::vector<McpTool>& tools) {
    std::string json = "[";
    for (size_t index = 0; index < tools.size(); ++index) {
        if (index) json += ",";
        const McpTool& tool = tools[index];
        json += "{\"name\":\"" + Esc(tool.name) + "\",\"description\":\"" +
                Esc(tool.description) +
                "\",\"input_schema\":" + tool.input_schema_json + "}";
    }
    json += "]";
    return json;
}

std::string BuildRequestBody(const ChatLlmConfig& config,
                             const std::vector<ChatMessage>& messages,
                             const std::string& systemPrompt,
                             const std::vector<McpTool>& tools) {
    return "{\"model\":\"" + Esc(config.model) +
           "\",\"max_tokens\":" + std::to_string(config.max_tokens) +
           ",\"system\":\"" + Esc(systemPrompt) +
           "\",\"messages\":" + DumpMessages(messages) +
           ",\"tools\":" + DumpTools(tools) + "}";
}

// Anthropic's error envelope:
// {"type":"error","error":{"type":...,"message":...}}. Falls back to the raw
// body when it doesn't match (matching sidecar/planner.py's _post_json, which
// reports the raw body text too).
std::string DescribeErrorBody(int statusCode, const std::string& body) {
    Value parsed;
    std::string parseError;
    std::string detail = body;
    if (Value::Parse(body, parsed, parseError) && parsed.IsObject()) {
        const Value* errorField = parsed.Find("error");
        const Value* messageField =
            errorField ? errorField->Find("message") : nullptr;
        if (messageField && messageField->IsString())
            detail = messageField->AsString();
    }
    return "HTTP " + std::to_string(statusCode) +
           " from the Anthropic API: " + detail;
}

bool ParseAssistantContent(const Value& contentField,
                           std::vector<ChatContentBlock>& content,
                           std::string& error) {
    if (!contentField.IsArray()) {
        error = "response 'content' is not an array";
        return false;
    }
    for (const Value& blockValue : contentField.AsArray()) {
        const Value* typeField = blockValue.Find("type");
        if (!typeField || !typeField->IsString()) {
            error = "a content block is missing its 'type'";
            return false;
        }
        const std::string& blockType = typeField->AsString();
        if (blockType == "text") {
            const Value* textField = blockValue.Find("text");
            content.push_back(ChatContentBlock::MakeText(
                textField && textField->IsString() ? textField->AsString()
                                                   : std::string()));
        } else if (blockType == "tool_use") {
            const Value* idField = blockValue.Find("id");
            const Value* nameField = blockValue.Find("name");
            const Value* inputField = blockValue.Find("input");
            if (!idField || !idField->IsString() || !nameField ||
                !nameField->IsString()) {
                error = "a tool_use block is missing 'id' or 'name'";
                return false;
            }
            content.push_back(ChatContentBlock::MakeToolUse(
                idField->AsString(), nameField->AsString(),
                inputField ? *inputField : Value::MakeObject()));
        }
        // Other block types (e.g. "thinking") are neither text nor a tool
        // call the chat panel acts on; skip rather than fail the turn.
    }
    return true;
}

}  // namespace

ChatContentBlock ChatContentBlock::MakeText(std::string text) {
    ChatContentBlock block;
    block.type = ChatBlockType::Text;
    block.text = std::move(text);
    return block;
}

ChatContentBlock ChatContentBlock::MakeToolUse(std::string id, std::string name,
                                               mcp_json::Value input) {
    ChatContentBlock block;
    block.type = ChatBlockType::ToolUse;
    block.tool_use_id = std::move(id);
    block.tool_name = std::move(name);
    block.tool_input = std::move(input);
    return block;
}

ChatContentBlock ChatContentBlock::MakeToolResult(std::string toolUseId,
                                                  std::string text,
                                                  bool isError) {
    ChatContentBlock block;
    block.type = ChatBlockType::ToolResult;
    block.tool_use_id = std::move(toolUseId);
    block.text = std::move(text);
    block.tool_is_error = isError;
    return block;
}

ChatLlmConfig ChatLlmConfig::FromEnvironment(std::string* missingKeyError) {
    LoadLocalEnvFileIfPresent();

    ChatLlmConfig config;
    const std::string explicitModel = EnvString("CUTMACHINE_ANTHROPIC_MODEL");
    const std::string genericModel = EnvString("CUTMACHINE_MODEL");
    config.model = !explicitModel.empty()  ? explicitModel
                   : !genericModel.empty() ? genericModel
                                           : config.model;
    config.api_key = EnvString("ANTHROPIC_API_KEY");
    const std::string baseUrl = EnvString("CUTMACHINE_ANTHROPIC_URL");
    if (!baseUrl.empty()) config.base_url = baseUrl;

    if (config.api_key.empty() && missingKeyError) {
        *missingKeyError =
            "ANTHROPIC_API_KEY is not set. CUTMACHINE talks to Anthropic "
            "directly with your own key (PHILOSOPHY.md: pas de service "
            "central) -- export ANTHROPIC_API_KEY, or put it in "
            "~/.config/cutmachine/.env, or point CUTMACHINE_ENV_FILE at a "
            "file that sets it.";
    }
    return config;
}

ChatLlmClient::ChatLlmClient(ChatLlmConfig config, ChatHttpTransport transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

ChatSendResult ChatLlmClient::SendMessages(
    const std::vector<ChatMessage>& messages, const std::string& systemPrompt,
    const std::vector<McpTool>& tools) const {
    ChatSendResult result;
    if (config_.api_key.empty()) {
        result.error =
            "ANTHROPIC_API_KEY is not set; see ChatLlmConfig::FromEnvironment.";
        return result;
    }

    ChatHttpRequest request;
    request.url = config_.base_url + "/v1/messages";
    request.headers = {
        {"Content-Type", "application/json"},
        {"x-api-key", config_.api_key},
        {"anthropic-version", config_.anthropic_version},
    };
    request.body = BuildRequestBody(config_, messages, systemPrompt, tools);

    ChatHttpResponse response;
    std::string transportError;
    if (!transport_) {
        result.error = "no HTTP transport configured";
        return result;
    }
    if (!transport_(request, response, transportError)) {
        result.error = "unable to reach " + request.url + ": " + transportError;
        return result;
    }
    if (response.status_code != 200) {
        result.error = DescribeErrorBody(response.status_code, response.body);
        return result;
    }

    Value parsed;
    std::string parseError;
    if (!Value::Parse(response.body, parsed, parseError)) {
        result.error =
            "the Anthropic API returned malformed JSON: " + parseError;
        return result;
    }
    if (!parsed.IsObject()) {
        result.error = "the Anthropic API response is not a JSON object";
        return result;
    }
    const Value* contentField = parsed.Find("content");
    if (!contentField) {
        result.error = "the Anthropic API response has no 'content'";
        return result;
    }
    std::string contentError;
    if (!ParseAssistantContent(*contentField, result.content, contentError)) {
        result.error =
            "the Anthropic API response is malformed: " + contentError;
        return result;
    }
    const Value* stopReasonField = parsed.Find("stop_reason");
    if (stopReasonField && stopReasonField->IsString())
        result.stop_reason = stopReasonField->AsString();
    result.ok = true;
    return result;
}

}  // namespace chat

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
        case ChatBlockType::ToolResult: {
            // A tool result carrying a picture becomes an array of blocks --
            // the text the tool returned, then the image. Anthropic accepts
            // an image inside a tool_result; the plain-string form stays for
            // every other tool so nothing about the existing traffic
            // changes.
            const std::string body =
                block.tool_image_base64.empty() || block.tool_image_mime.empty()
                    ? "\"" + Esc(block.text) + "\""
                    : "[{\"type\":\"text\",\"text\":\"" + Esc(block.text) +
                          "\"},{\"type\":\"image\",\"source\":{\"type\":"
                          "\"base64\",\"media_type\":\"" +
                          Esc(block.tool_image_mime) + "\",\"data\":\"" +
                          Esc(block.tool_image_base64) + "\"}}]";
            return "{\"type\":\"tool_result\",\"tool_use_id\":\"" +
                   Esc(block.tool_use_id) + "\",\"content\":" + body +
                   ",\"is_error\":" + (block.tool_is_error ? "true" : "false") +
                   "}";
        }
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

std::string BuildAnthropicRequestBody(const ChatLlmConfig& config,
                                      const std::vector<ChatMessage>& messages,
                                      const std::string& systemPrompt,
                                      const std::vector<McpTool>& tools) {
    return "{\"model\":\"" + Esc(config.model) +
           "\",\"max_tokens\":" + std::to_string(config.max_tokens) +
           ",\"system\":\"" + Esc(systemPrompt) +
           "\",\"messages\":" + DumpMessages(messages) +
           ",\"tools\":" + DumpTools(tools) + "}";
}

std::string OpenAiTextContent(const ChatMessage& message) {
    std::string text;
    for (const ChatContentBlock& block : message.content) {
        if (block.type != ChatBlockType::Text) continue;
        if (!text.empty()) text += "\n";
        text += block.text;
    }
    return text;
}

std::string DumpOpenAiAssistantMessage(const ChatMessage& message) {
    const std::string text = OpenAiTextContent(message);
    std::string json = "{\"role\":\"assistant\",\"content\":";
    json += text.empty() ? "null" : "\"" + Esc(text) + "\"";
    bool firstTool = true;
    for (const ChatContentBlock& block : message.content) {
        if (block.type != ChatBlockType::ToolUse) continue;
        if (firstTool) json += ",\"tool_calls\":[";
        if (!firstTool) json += ",";
        firstTool = false;
        json += "{\"id\":\"" + Esc(block.tool_use_id) +
                "\",\"type\":\"function\",\"function\":{\"name\":\"" +
                Esc(block.tool_name) + "\",\"arguments\":\"" +
                Esc(block.tool_input.Dump()) + "\"}}";
    }
    if (!firstTool) json += "]";
    return json + "}";
}

std::string DumpOpenAiMessages(const std::vector<ChatMessage>& messages,
                               const std::string& systemPrompt) {
    std::string json =
        "[{\"role\":\"system\",\"content\":\"" + Esc(systemPrompt) + "\"}";
    for (const ChatMessage& message : messages) {
        if (message.role == "assistant") {
            json += "," + DumpOpenAiAssistantMessage(message);
            continue;
        }
        const std::string text = OpenAiTextContent(message);
        if (!text.empty())
            json += ",{\"role\":\"user\",\"content\":\"" + Esc(text) + "\"}";
        for (const ChatContentBlock& block : message.content) {
            if (block.type != ChatBlockType::ToolResult) continue;
            json += ",{\"role\":\"tool\",\"tool_call_id\":\"" +
                    Esc(block.tool_use_id) + "\",\"content\":\"" +
                    Esc(block.text) + "\"}";
        }
    }
    return json + "]";
}

std::string DumpOpenAiTools(const std::vector<McpTool>& tools) {
    std::string json = "[";
    for (size_t index = 0; index < tools.size(); ++index) {
        if (index) json += ",";
        const McpTool& tool = tools[index];
        json += "{\"type\":\"function\",\"function\":{\"name\":\"" +
                Esc(tool.name) + "\",\"description\":\"" +
                Esc(tool.description) +
                "\",\"parameters\":" + tool.input_schema_json + "}}";
    }
    return json + "]";
}

std::string BuildOpenAiRequestBody(const ChatLlmConfig& config,
                                   const std::vector<ChatMessage>& messages,
                                   const std::string& systemPrompt,
                                   const std::vector<McpTool>& tools) {
    std::string json =
        "{\"model\":\"" + Esc(config.model) +
        "\",\"max_tokens\":" + std::to_string(config.max_tokens) +
        ",\"messages\":" + DumpOpenAiMessages(messages, systemPrompt);
    if (!tools.empty())
        json += ",\"tools\":" + DumpOpenAiTools(tools) +
                ",\"tool_choice\":\"auto\"";
    return json + "}";
}

std::string DumpOllamaAssistantMessage(const ChatMessage& message) {
    const std::string text = OpenAiTextContent(message);
    std::string json =
        "{\"role\":\"assistant\",\"content\":\"" + Esc(text) + "\"";
    bool firstTool = true;
    for (const ChatContentBlock& block : message.content) {
        if (block.type != ChatBlockType::ToolUse) continue;
        if (firstTool) json += ",\"tool_calls\":[";
        if (!firstTool) json += ",";
        firstTool = false;
        json += "{\"type\":\"function\",\"function\":{\"name\":\"" +
                Esc(block.tool_name) +
                "\",\"arguments\":" + block.tool_input.Dump() + "}}";
    }
    if (!firstTool) json += "]";
    return json + "}";
}

std::string DumpOllamaMessages(const std::vector<ChatMessage>& messages,
                               const std::string& systemPrompt) {
    std::string json =
        "[{\"role\":\"system\",\"content\":\"" + Esc(systemPrompt) + "\"}";
    for (const ChatMessage& message : messages) {
        if (message.role == "assistant") {
            json += "," + DumpOllamaAssistantMessage(message);
            continue;
        }
        const std::string text = OpenAiTextContent(message);
        if (!text.empty())
            json += ",{\"role\":\"user\",\"content\":\"" + Esc(text) + "\"}";
        for (const ChatContentBlock& block : message.content) {
            if (block.type != ChatBlockType::ToolResult) continue;
            json += ",{\"role\":\"tool\",\"tool_name\":\"" +
                    Esc(block.tool_name) + "\",\"content\":\"" +
                    Esc(block.text) + "\"}";
        }
    }
    return json + "]";
}

std::string BuildOllamaRequestBody(const ChatLlmConfig& config,
                                   const std::vector<ChatMessage>& messages,
                                   const std::string& systemPrompt,
                                   const std::vector<McpTool>& tools) {
    std::string json =
        "{\"model\":\"" + Esc(config.model) +
        "\",\"stream\":false,\"options\":{\"temperature\":0," +
        "\"num_ctx\":" + std::to_string(config.context_tokens) +
        "},\"messages\":" + DumpOllamaMessages(messages, systemPrompt);
    if (!tools.empty()) json += ",\"tools\":" + DumpOpenAiTools(tools);
    return json + "}";
}

// Anthropic's error envelope:
// {"type":"error","error":{"type":...,"message":...}}. Falls back to the raw
// body when it doesn't match (matching sidecar/planner.py's _post_json, which
// reports the raw body text too).
std::string DescribeErrorBody(int statusCode, const std::string& body,
                              const std::string& apiName) {
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
    return "HTTP " + std::to_string(statusCode) + " from " + apiName + ": " +
           detail;
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

bool ParseTextToolCall(const std::string& text,
                       const std::vector<McpTool>& tools,
                       ChatContentBlock& block) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(text, parsed, parseError) || !parsed.IsObject() ||
        parsed.AsObject().size() != 2)
        return false;
    const Value* name = parsed.Find("name");
    const Value* arguments = parsed.Find("arguments");
    if (!name || !name->IsString() || !arguments || !arguments->IsObject())
        return false;
    bool known = false;
    for (const McpTool& tool : tools) known |= tool.name == name->AsString();
    if (!known) return false;
    block = ChatContentBlock::MakeToolUse("ollama_text_tool", name->AsString(),
                                          *arguments);
    return true;
}

bool ParseOpenAiAssistant(const Value& root, const std::vector<McpTool>& tools,
                          ChatSendResult& result, std::string& error) {
    const Value* choices = root.Find("choices");
    if (!choices || !choices->IsArray() || choices->AsArray().empty()) {
        error = "response has no non-empty 'choices' array";
        return false;
    }
    const Value& choice = choices->AsArray().front();
    const Value* message = choice.Find("message");
    if (!message || !message->IsObject()) {
        error = "first choice has no 'message' object";
        return false;
    }
    const Value* toolCalls = message->Find("tool_calls");
    if (toolCalls) {
        if (!toolCalls->IsArray()) {
            error = "message 'tool_calls' is not an array";
            return false;
        }
        for (const Value& call : toolCalls->AsArray()) {
            const Value* id = call.Find("id");
            const Value* function = call.Find("function");
            const Value* name = function ? function->Find("name") : nullptr;
            const Value* arguments =
                function ? function->Find("arguments") : nullptr;
            if (!id || !id->IsString() || !name || !name->IsString() ||
                !arguments || !arguments->IsString()) {
                error = "a tool call is missing id, function name or arguments";
                return false;
            }
            Value input;
            std::string parseError;
            if (!Value::Parse(arguments->AsString(), input, parseError)) {
                error = "tool arguments are malformed JSON: " + parseError;
                return false;
            }
            result.content.push_back(ChatContentBlock::MakeToolUse(
                id->AsString(), name->AsString(), std::move(input)));
        }
    }
    const Value* text = message->Find("content");
    if (text && text->IsString() && !text->AsString().empty()) {
        ChatContentBlock textTool;
        if (!toolCalls && ParseTextToolCall(text->AsString(), tools, textTool))
            result.content.push_back(std::move(textTool));
        else
            result.content.push_back(
                ChatContentBlock::MakeText(text->AsString()));
    }
    const Value* finishReason = choice.Find("finish_reason");
    if (finishReason && finishReason->IsString()) {
        result.stop_reason = finishReason->AsString() == "tool_calls"
                                 ? "tool_use"
                                 : finishReason->AsString();
    }
    return true;
}

bool ParseOllamaAssistant(const Value& root, const std::vector<McpTool>& tools,
                          ChatSendResult& result, std::string& error) {
    const Value* message = root.Find("message");
    if (!message || !message->IsObject()) {
        error = "response has no 'message' object";
        return false;
    }
    const Value* toolCalls = message->Find("tool_calls");
    if (toolCalls) {
        if (!toolCalls->IsArray()) {
            error = "message 'tool_calls' is not an array";
            return false;
        }
        size_t index = 0;
        for (const Value& call : toolCalls->AsArray()) {
            const Value* function = call.Find("function");
            const Value* name = function ? function->Find("name") : nullptr;
            const Value* arguments =
                function ? function->Find("arguments") : nullptr;
            if (!name || !name->IsString() || !arguments ||
                !arguments->IsObject()) {
                error = "a tool call is missing function name or arguments";
                return false;
            }
            result.content.push_back(ChatContentBlock::MakeToolUse(
                "ollama_tool_" + std::to_string(index++), name->AsString(),
                *arguments));
        }
    }
    const Value* text = message->Find("content");
    if (text && text->IsString() && !text->AsString().empty()) {
        ChatContentBlock textTool;
        if (!toolCalls && ParseTextToolCall(text->AsString(), tools, textTool))
            result.content.push_back(std::move(textTool));
        else
            result.content.push_back(
                ChatContentBlock::MakeText(text->AsString()));
    }
    const Value* doneReason = root.Find("done_reason");
    result.stop_reason = toolCalls ? "tool_use" : "end_turn";
    if (!toolCalls && doneReason && doneReason->IsString())
        result.stop_reason = doneReason->AsString();
    if (!result.content.empty() &&
        result.content.back().type == ChatBlockType::ToolUse)
        result.stop_reason = "tool_use";
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

ChatContentBlock ChatContentBlock::MakeToolResult(
    std::string toolUseId, std::string text, bool isError, std::string toolName,
    std::string imageBase64, std::string imageMime) {
    ChatContentBlock block;
    block.type = ChatBlockType::ToolResult;
    block.tool_use_id = std::move(toolUseId);
    block.text = std::move(text);
    block.tool_name = std::move(toolName);
    block.tool_is_error = isError;
    block.tool_image_base64 = std::move(imageBase64);
    block.tool_image_mime = std::move(imageMime);
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
    if (config_.provider == ChatLlmProvider::AnthropicMessages &&
        config_.api_key.empty()) {
        result.error =
            "ANTHROPIC_API_KEY is not set; see ChatLlmConfig::FromEnvironment.";
        return result;
    }

    ChatHttpRequest request;
    request.headers = {{"Content-Type", "application/json"}};
    if (config_.provider == ChatLlmProvider::AnthropicMessages) {
        request.url = config_.base_url + "/v1/messages";
        request.headers.push_back({"x-api-key", config_.api_key});
        request.headers.push_back(
            {"anthropic-version", config_.anthropic_version});
        request.body =
            BuildAnthropicRequestBody(config_, messages, systemPrompt, tools);
    } else if (config_.provider == ChatLlmProvider::OpenAiCompatible) {
        request.url = config_.base_url + "/chat/completions";
        if (!config_.api_key.empty())
            request.headers.push_back(
                {"Authorization", "Bearer " + config_.api_key});
        request.body =
            BuildOpenAiRequestBody(config_, messages, systemPrompt, tools);
    } else {
        request.url = config_.base_url + "/api/chat";
        request.body =
            BuildOllamaRequestBody(config_, messages, systemPrompt, tools);
    }

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
        const std::string apiName =
            config_.provider == ChatLlmProvider::AnthropicMessages
                ? "the Anthropic API"
            : config_.provider == ChatLlmProvider::Ollama
                ? "the Ollama API"
                : "the OpenAI-compatible API";
        result.error =
            DescribeErrorBody(response.status_code, response.body, apiName);
        return result;
    }

    Value parsed;
    std::string parseError;
    if (!Value::Parse(response.body, parsed, parseError)) {
        result.error = "the model API returned malformed JSON: " + parseError;
        return result;
    }
    if (!parsed.IsObject()) {
        result.error = "the model API response is not a JSON object";
        return result;
    }
    std::string contentError;
    if (config_.provider == ChatLlmProvider::AnthropicMessages) {
        const Value* contentField = parsed.Find("content");
        if (!contentField) {
            result.error = "the Anthropic API response has no 'content'";
            return result;
        }
        if (!ParseAssistantContent(*contentField, result.content,
                                   contentError)) {
            result.error =
                "the Anthropic API response is malformed: " + contentError;
            return result;
        }
        const Value* stopReasonField = parsed.Find("stop_reason");
        if (stopReasonField && stopReasonField->IsString())
            result.stop_reason = stopReasonField->AsString();
    } else if (config_.provider == ChatLlmProvider::OpenAiCompatible &&
               !ParseOpenAiAssistant(parsed, tools, result, contentError)) {
        result.error =
            "the OpenAI-compatible response is malformed: " + contentError;
        return result;
    } else if (config_.provider == ChatLlmProvider::Ollama &&
               !ParseOllamaAssistant(parsed, tools, result, contentError)) {
        result.error = "the Ollama response is malformed: " + contentError;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace chat

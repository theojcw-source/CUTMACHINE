// Pure-C++ test for the model API client (ROADMAP.md F2.4).
// Depends only on Json.h/McpTools.h/ChatLlmClient.h -- no AppKit, no
// sockets, no real network -- so it builds and runs on a plain Linux host,
// the same way tests/mcp_tools_tests.cc does for the MCP tool dispatcher.
// The real HTTPS transport (NSURLSession-backed) lives in ChatPanelView.mm
// and is AppKit-only, not exercised at runtime yet; everything this file
// exercises -- request assembly, response parsing, error handling -- is
// exactly what that transport is a thin, swappable I/O shim around.

#include "ChatLlmClient.h"
#include "Json.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using chat::ChatBlockType;
using chat::ChatContentBlock;
using chat::ChatHttpRequest;
using chat::ChatHttpResponse;
using chat::ChatLlmClient;
using chat::ChatLlmConfig;
using chat::ChatMessage;
using chat::ChatSendResult;
using mcp_json::Value;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) std::cout << "PASS: " << name << '\n';
}

ChatLlmConfig TestConfig() {
    ChatLlmConfig config;
    config.model = "claude-sonnet-4-5";
    config.api_key = "sk-test-key";
    config.base_url = "https://api.example.invalid";
    return config;
}

ChatLlmConfig OllamaConfig() {
    ChatLlmConfig config;
    config.provider = chat::ChatLlmProvider::OpenAiCompatible;
    config.model = "qwen2.5-coder:7b";
    config.base_url = "http://localhost:11434/v1";
    return config;
}

ChatLlmConfig OllamaNativeConfig() {
    ChatLlmConfig config;
    config.provider = chat::ChatLlmProvider::Ollama;
    config.model = "qwen2.5-coder:7b";
    config.base_url = "http://localhost:11434";
    config.context_tokens = 32768;
    return config;
}

std::vector<McpTool> OneToolCatalog() {
    return {{"trim_clip", "Trim a clip.",
             "{\"type\":\"object\",\"properties\":{\"clip_id\":{\"type\":"
             "\"string\"}},\"required\":[\"clip_id\"],\"additionalProperties\":"
             "false}"}};
}

}  // namespace

int main() {
    Test("SendMessages builds the Anthropic request shape and headers", [] {
        ChatHttpRequest captured;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body =
                R"({"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        ChatMessage user;
        user.role = "user";
        user.content.push_back(
            ChatContentBlock::MakeText("Coupe le premier plan."));
        const ChatSendResult result =
            client.SendMessages({user}, "Tu es un agent.", OneToolCatalog());

        Check(result.ok, "the call succeeds: " + result.error);
        Check(
            captured.url == "https://api.example.invalid/v1/messages",
            "the request targets base_url + /v1/messages, got " + captured.url);

        bool sawApiKeyHeader = false;
        bool sawVersionHeader = false;
        for (const auto& header : captured.headers) {
            if (header.first == "x-api-key")
                sawApiKeyHeader = header.second == "sk-test-key";
            if (header.first == "anthropic-version")
                sawVersionHeader = header.second == "2023-06-01";
        }
        Check(sawApiKeyHeader, "x-api-key carries the configured key");
        Check(sawVersionHeader, "anthropic-version is set");

        Value body;
        std::string parseError;
        Check(Value::Parse(captured.body, body, parseError),
              "request body is valid JSON: " + parseError);
        const Value* model = body.Find("model");
        Check(model && model->IsString() &&
                  model->AsString() == "claude-sonnet-4-5",
              "request body carries the configured model");
        const Value* system = body.Find("system");
        Check(system && system->IsString() &&
                  system->AsString() == "Tu es un agent.",
              "request body carries the system prompt");
        const Value* tools = body.Find("tools");
        Check(tools && tools->IsArray() && tools->AsArray().size() == 1,
              "request body carries the tool catalog");
        if (tools && tools->IsArray() && tools->AsArray().size() == 1) {
            const Value& tool = tools->AsArray()[0];
            const Value* name = tool.Find("name");
            Check(name && name->IsString() && name->AsString() == "trim_clip",
                  "the tool entry names trim_clip");
            const Value* schema = tool.Find("input_schema");
            Check(schema && schema->IsObject(),
                  "the tool entry's input_schema round-trips as an object");
        }
        const Value* messages = body.Find("messages");
        Check(
            messages && messages->IsArray() && messages->AsArray().size() == 1,
            "request body carries exactly the one user message");
        if (messages && messages->IsArray() &&
            messages->AsArray().size() == 1) {
            const Value& message = messages->AsArray()[0];
            const Value* role = message.Find("role");
            Check(role && role->IsString() && role->AsString() == "user",
                  "the message role is user");
            const Value* content = message.Find("content");
            Check(
                content && content->IsArray() && content->AsArray().size() == 1,
                "the message has one content block");
        }
    });

    Test("a text-only response parses to one Text block and end_turn", [] {
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body = R"({"content":[{"type":"text","text":"Bonjour."}],)"
                            R"("stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.stop_reason == "end_turn", "stop_reason is end_turn");
        Check(result.content.size() == 1, "exactly one content block");
        if (result.content.size() == 1) {
            Check(result.content[0].type == ChatBlockType::Text,
                  "the block is text");
            Check(result.content[0].text == "Bonjour.", "text matches");
        }
    });

    Test("a tool_use response parses id/name/input and reports tool_use", [] {
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body =
                R"({"content":[)"
                R"({"type":"text","text":"D'accord."},)"
                R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
                R"("input":{"clip_id":"01K300000000000000000003","edge":"Tail",)"
                R"("delta":{"value":-1,"rate":25}}})"
                R"(],"stop_reason":"tool_use"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.stop_reason == "tool_use", "stop_reason is tool_use");
        Check(result.content.size() == 2, "two content blocks");
        if (result.content.size() == 2) {
            Check(result.content[0].type == ChatBlockType::Text,
                  "first is text");
            Check(result.content[1].type == ChatBlockType::ToolUse,
                  "second is tool_use");
            Check(result.content[1].tool_use_id == "toolu_1",
                  "tool_use id matches");
            Check(result.content[1].tool_name == "trim_clip",
                  "tool name matches");
            const Value* clipId = result.content[1].tool_input.Find("clip_id");
            Check(clipId && clipId->IsString() &&
                      clipId->AsString() == "01K300000000000000000003",
                  "tool input round-trips clip_id");
        }
    });

    Test("a tool_use and tool_result block round-trip through the request body",
         [] {
             ChatHttpRequest captured;
             auto transport = [&](const ChatHttpRequest& request,
                                  ChatHttpResponse& response, std::string&) {
                 captured = request;
                 response.status_code = 200;
                 response.body =
                     R"({"content":[{"type":"text","text":"fait."}],)"
                     R"("stop_reason":"end_turn"})";
                 return true;
             };
             ChatLlmClient client(TestConfig(), transport);

             Value input = Value::MakeObject();
             input.Set("clip_id", Value::MakeString("01K3X"));
             ChatMessage assistant;
             assistant.role = "assistant";
             assistant.content.push_back(
                 ChatContentBlock::MakeToolUse("toolu_1", "trim_clip", input));
             ChatMessage toolResults;
             toolResults.role = "user";
             toolResults.content.push_back(ChatContentBlock::MakeToolResult(
                 "toolu_1", "{\"ok\":true}", false));

             client.SendMessages({assistant, toolResults}, "sys", {});

             Value body;
             std::string parseError;
             Check(Value::Parse(captured.body, body, parseError),
                   "request body is valid JSON: " + parseError);
             const Value* messages = body.Find("messages");
             Check(messages && messages->IsArray() &&
                       messages->AsArray().size() == 2,
                   "both messages are present");
             if (messages && messages->AsArray().size() == 2) {
                 const Value& firstContent =
                     *messages->AsArray()[0].Find("content");
                 const Value& toolUseBlock = firstContent.AsArray()[0];
                 const Value* toolUseType = toolUseBlock.Find("type");
                 Check(toolUseType && toolUseType->AsString() == "tool_use",
                       "assistant message's block is tool_use");
                 const Value* toolUseInput = toolUseBlock.Find("input");
                 const Value* clipId =
                     toolUseInput ? toolUseInput->Find("clip_id") : nullptr;
                 Check(clipId && clipId->AsString() == "01K3X",
                       "tool_use input round-trips through Dump()");

                 const Value& secondContent =
                     *messages->AsArray()[1].Find("content");
                 const Value& toolResultBlock = secondContent.AsArray()[0];
                 const Value* toolResultType = toolResultBlock.Find("type");
                 Check(toolResultType &&
                           toolResultType->AsString() == "tool_result",
                       "second message's block is tool_result");
                 const Value* isError = toolResultBlock.Find("is_error");
                 Check(
                     isError && isError->IsBool() && isError->AsBool() == false,
                     "tool_result carries is_error:false");
             }
         });

    Test(
        "a non-200 status is reported as an error, not a crash or false ok",
        [] {
            auto transport = [](const ChatHttpRequest&,
                                ChatHttpResponse& response, std::string&) {
                response.status_code = 401;
                response.body =
                    R"({"type":"error","error":{"type":"authentication_error",)"
                    R"("message":"invalid x-api-key"}})";
                return true;
            };
            ChatLlmClient client(TestConfig(), transport);
            const ChatSendResult result = client.SendMessages({}, "sys", {});
            Check(!result.ok, "the call is reported as failed");
            Check(result.error.find("invalid x-api-key") != std::string::npos,
                  "the error surfaces the API's own message, got: " +
                      result.error);
        });

    Test("malformed JSON in a 200 response is reported as an error", [] {
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body = "not json";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(!result.ok, "the call is reported as failed");
        Check(!result.error.empty(), "an error message is set");
    });

    Test(
        "a transport-level failure is reported without touching the response "
        "body",
        [] {
            auto transport = [](const ChatHttpRequest&, ChatHttpResponse&,
                                std::string& error) {
                error = "connection refused";
                return false;
            };
            ChatLlmClient client(TestConfig(), transport);
            const ChatSendResult result = client.SendMessages({}, "sys", {});
            Check(!result.ok, "the call is reported as failed");
            Check(
                result.error.find("connection refused") != std::string::npos,
                "the transport's own error is surfaced, got: " + result.error);
        });

    Test(
        "Ollama uses OpenAI chat completions without requiring an API key", [] {
            ChatHttpRequest captured;
            auto transport = [&](const ChatHttpRequest& request,
                                 ChatHttpResponse& response, std::string&) {
                captured = request;
                response.status_code = 200;
                response.body = R"({"choices":[{"message":{"role":"assistant",)"
                                R"("content":"Bonjour depuis Qwen."},)"
                                R"("finish_reason":"stop"}]})";
                return true;
            };
            ChatMessage user;
            user.role = "user";
            user.content.push_back(ChatContentBlock::MakeText("Bonjour"));
            ChatLlmClient client(OllamaConfig(), transport);
            const ChatSendResult result =
                client.SendMessages({user}, "Tu montes.", OneToolCatalog());

            Check(result.ok, "the Ollama call succeeds: " + result.error);
            Check(captured.url == "http://localhost:11434/v1/chat/completions",
                  "the request targets Ollama's OpenAI endpoint");
            bool sawAuthorization = false;
            for (const auto& header : captured.headers)
                sawAuthorization |= header.first == "Authorization";
            Check(!sawAuthorization,
                  "a local Ollama request sends no fake credential");

            Value body;
            std::string parseError;
            Check(
                Value::Parse(captured.body, body, parseError),
                "OpenAI-compatible request body is valid JSON: " + parseError);
            const Value* messages = body.Find("messages");
            Check(messages && messages->IsArray() &&
                      messages->AsArray().size() == 2,
                  "system and user messages are sent");
            const Value* tools = body.Find("tools");
            Check(tools && tools->IsArray() && tools->AsArray().size() == 1,
                  "MCP tools use the OpenAI function schema");
            Check(result.content.size() == 1 &&
                      result.content.front().text == "Bonjour depuis Qwen.",
                  "the OpenAI text response is parsed");
        });

    Test("OpenAI tool calls and tool results bridge the agent loop", [] {
        ChatHttpRequest captured;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body =
                R"({"choices":[{"message":{"role":"assistant",)"
                R"("content":null,"tool_calls":[{"id":"call_1",)"
                R"("type":"function","function":{"name":"trim_clip",)"
                R"("arguments":"{\"clip_id\":\"01K3X\"}"}}]},)"
                R"("finish_reason":"tool_calls"}]})";
            return true;
        };
        ChatLlmClient client(OllamaConfig(), transport);
        const ChatSendResult result =
            client.SendMessages({}, "sys", OneToolCatalog());
        Check(result.ok, "the tool response succeeds: " + result.error);
        Check(result.stop_reason == "tool_use",
              "OpenAI tool_calls maps to the session's tool_use reason");
        Check(result.content.size() == 1 &&
                  result.content.front().type == ChatBlockType::ToolUse,
              "the function call becomes a ToolUse block");
        if (!result.content.empty()) {
            const Value* clipId =
                result.content.front().tool_input.Find("clip_id");
            Check(clipId && clipId->IsString() && clipId->AsString() == "01K3X",
                  "function arguments parse as structured tool input");
        }

        Value input = Value::MakeObject();
        input.Set("clip_id", Value::MakeString("01K3X"));
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content.push_back(
            ChatContentBlock::MakeToolUse("call_1", "trim_clip", input));
        ChatMessage toolResult;
        toolResult.role = "user";
        toolResult.content.push_back(
            ChatContentBlock::MakeToolResult("call_1", "{\"ok\":true}", false));
        client.SendMessages({assistant, toolResult}, "sys", {});

        Value body;
        std::string parseError;
        Check(Value::Parse(captured.body, body, parseError),
              "follow-up request body is valid JSON: " + parseError);
        const Value* messages = body.Find("messages");
        Check(
            messages && messages->IsArray() && messages->AsArray().size() == 3,
            "system, assistant tool call and tool result are sent");
        if (messages && messages->IsArray() &&
            messages->AsArray().size() == 3) {
            const Value* role = messages->AsArray()[2].Find("role");
            Check(role && role->IsString() && role->AsString() == "tool",
                  "tool result uses the OpenAI tool role");
        }
    });

    Test(
        "native Ollama requests a larger context and accepts Qwen textual "
        "tools",
        [] {
            ChatHttpRequest captured;
            auto transport = [&](const ChatHttpRequest& request,
                                 ChatHttpResponse& response, std::string&) {
                captured = request;
                response.status_code = 200;
                response.body =
                    R"({"message":{"role":"assistant","content":)"
                    R"("{\"name\":\"trim_clip\",\"arguments\":{\"clip_id\":\"01K3X\"}}"},)"
                    R"("done":true,"done_reason":"stop"})";
                return true;
            };
            ChatLlmClient client(OllamaNativeConfig(), transport);
            const ChatSendResult result =
                client.SendMessages({}, "sys", OneToolCatalog());

            Check(result.ok,
                  "the native Ollama call succeeds: " + result.error);
            Check(captured.url == "http://localhost:11434/api/chat",
                  "native Ollama targets /api/chat");
            Value body;
            std::string parseError;
            Check(Value::Parse(captured.body, body, parseError),
                  "native Ollama request is valid JSON: " + parseError);
            const Value* options = body.Find("options");
            const Value* context = options ? options->Find("num_ctx") : nullptr;
            int64_t contextTokens = 0;
            Check(context && context->AsInt64(contextTokens) &&
                      contextTokens == 32768,
                  "native Ollama receives the configured 32K context");
            Check(result.stop_reason == "tool_use" &&
                      result.content.size() == 1 &&
                      result.content.front().type == ChatBlockType::ToolUse,
                  "Qwen's strict textual tool object becomes a tool call");
            if (!result.content.empty())
                Check(result.content.front().tool_name == "trim_clip",
                      "the textual tool name is preserved");
        });

    Test("native Ollama history carries function calls and named tool results",
         [] {
             ChatHttpRequest captured;
             auto transport = [&](const ChatHttpRequest& request,
                                  ChatHttpResponse& response, std::string&) {
                 captured = request;
                 response.status_code = 200;
                 response.body =
                     R"({"message":{"role":"assistant","content":"fait"},)"
                     R"("done":true,"done_reason":"stop"})";
                 return true;
             };
             Value input = Value::MakeObject();
             input.Set("clip_id", Value::MakeString("01K3X"));
             ChatMessage assistant;
             assistant.role = "assistant";
             assistant.content.push_back(ChatContentBlock::MakeToolUse(
                 "ollama_text_tool", "trim_clip", input));
             ChatMessage resultMessage;
             resultMessage.role = "user";
             resultMessage.content.push_back(ChatContentBlock::MakeToolResult(
                 "ollama_text_tool", "{\"ok\":true}", false, "trim_clip"));
             ChatLlmClient client(OllamaNativeConfig(), transport);
             const ChatSendResult result = client.SendMessages(
                 {assistant, resultMessage}, "sys", OneToolCatalog());
             Check(result.ok, "the follow-up succeeds: " + result.error);

             Value body;
             std::string parseError;
             Check(Value::Parse(captured.body, body, parseError),
                   "follow-up body is valid JSON: " + parseError);
             const Value* messages = body.Find("messages");
             Check(messages && messages->IsArray() &&
                       messages->AsArray().size() == 3,
                   "system, assistant and tool messages are present");
             if (messages && messages->IsArray() &&
                 messages->AsArray().size() == 3) {
                 const Value& tool = messages->AsArray()[2];
                 const Value* role = tool.Find("role");
                 const Value* name = tool.Find("tool_name");
                 Check(role && role->IsString() && role->AsString() == "tool" &&
                           name && name->IsString() &&
                           name->AsString() == "trim_clip",
                       "native tool result names the invoked function");
             }
         });

    Test("a missing API key short-circuits before the transport runs", [] {
        bool transportCalled = false;
        auto transport = [&](const ChatHttpRequest&, ChatHttpResponse& response,
                             std::string&) {
            transportCalled = true;
            response.status_code = 200;
            response.body = "{}";
            return true;
        };
        ChatLlmConfig config = TestConfig();
        config.api_key.clear();
        ChatLlmClient client(config, transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(!result.ok, "the call is reported as failed");
        Check(!transportCalled, "the transport is never invoked without a key");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    // QC-2026-08 -- a tool result carrying a picture must reach Anthropic as
    // an image block, not as a description of one. Without this the whole
    // read_frame path is a no-op the model never sees.
    {
        chat::ChatLlmConfig imageConfig;
        imageConfig.provider = chat::ChatLlmProvider::AnthropicMessages;
        imageConfig.model = "claude-sonnet-4-5";
        imageConfig.api_key = "test-key";
        imageConfig.base_url = "https://api.anthropic.test";
        std::string captured;
        chat::ChatLlmClient imageClient(
            imageConfig, [&captured](const ChatHttpRequest& request,
                                     ChatHttpResponse& response, std::string&) {
                captured = request.body;
                response.status_code = 200;
                response.body = R"({"content":[{"type":"text","text":"vu"}]})";
                return true;
            });
        chat::ChatMessage message;
        message.role = "user";
        message.content.push_back(chat::ChatContentBlock::MakeToolResult(
            "toolu_1", "{\"source_id\":\"M1\"}", false, "read_frame",
            "Zm9vYmFy", "image/jpeg"));
        std::vector<McpTool> noTools;
        const chat::ChatSendResult result =
            imageClient.SendMessages({message}, "system", noTools);
        Check(result.ok, "the image-carrying turn is sent: " + result.error);
        Check(captured.find("\"type\":\"image\"") != std::string::npos,
              "the request carries an image block");
        Check(
            captured.find("\"media_type\":\"image/jpeg\"") != std::string::npos,
            "with its MIME type");
        Check(captured.find("\"data\":\"Zm9vYmFy\"") != std::string::npos,
              "and the base64 payload itself");
        Check(captured.find("\"type\":\"text\",\"text\":\"{") !=
                  std::string::npos,
              "the tool's own text stays alongside the picture");

        // Every other tool result must keep the plain string form, so adding
        // pictures changed nothing about the traffic that already worked.
        std::string plainBody;
        chat::ChatLlmClient plainClient(
            imageConfig,
            [&plainBody](const ChatHttpRequest& request,
                         ChatHttpResponse& response, std::string&) {
                plainBody = request.body;
                response.status_code = 200;
                response.body = R"({"content":[{"type":"text","text":"ok"}]})";
                return true;
            });
        chat::ChatMessage plain;
        plain.role = "user";
        plain.content.push_back(chat::ChatContentBlock::MakeToolResult(
            "toolu_2", "{\"ok\":true}", false, "describe"));
        Check(plainClient.SendMessages({plain}, "system", noTools).ok,
              "a textual tool result still sends");
        Check(plainBody.find("\"type\":\"image\"") == std::string::npos &&
                  plainBody.find("\"content\":\"{") != std::string::npos,
              "a textual tool result keeps the plain string content form");
    }

    std::cout << "All chat LLM client tests passed\n";
    return 0;
}

// Pure-C++ test for the Anthropic Messages API client (ROADMAP.md F2.4).
// Depends only on Json.h/McpTools.h/ChatLlmClient.h -- no AppKit, no
// sockets, no real network -- so it builds and runs on a plain Linux host,
// the same way tests/mcp_tools_tests.cc does for the MCP tool dispatcher.
// The real HTTPS transport (NSURLSession-backed) lives in ChatPanelView.mm
// and is AppKit-only, unverified beyond a manual read; everything this file
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
    std::cout << "All chat LLM client tests passed\n";
    return 0;
}

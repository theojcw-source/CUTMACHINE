#pragma once

// LLM client for the chat panel (ROADMAP.md F2.4).
//
// PHILOSOPHY.md's "pas de service central" non-but, unchanged by the F2.x
// amendment: CUTMACHINE never runs or proxies a model backend. This talks
// directly to Anthropic or an OpenAI-compatible endpoint such as a local
// Ollama server -- never a CUTMACHINE-operated account or credit balance.
// The Anthropic mode mirrors sidecar/planner.py's conventions, so a key or
// override already set up for the Python sidecar works unchanged:
// ANTHROPIC_API_KEY, CUTMACHINE_ANTHROPIC_MODEL (falling back to the
// sidecar's generic CUTMACHINE_MODEL), CUTMACHINE_ANTHROPIC_URL. See
// ChatLlmConfig::FromEnvironment.
//
// This file and ChatLlmClient.cc are plain C++ (Json.h/McpTools.h only, no
// AppKit, no sockets). The actual HTTPS request/response is an injected
// `ChatHttpTransport` function, so the request-building and response-
// parsing logic here is unit-testable against a fake transport without a
// network or a real API key (tests/chat_llm_client_tests.cc). The one real
// transport -- NSURLSession-backed, since this project has no other outbound
// dependency to reach for -- lives in ChatPanelView.mm and is AppKit-only,
// not exercised at runtime yet like the rest of this project's AppKit
// surface.

#include "Json.h"
#include "McpTools.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace chat {

enum class ChatLlmProvider { AnthropicMessages, OpenAiCompatible, Ollama };

// One HTTPS POST request/response pair. Deliberately just strings and a
// status code -- everything about *how* the bytes get there (TLS, sockets,
// NSURLSession) is the transport's problem, not this client's.
struct ChatHttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct ChatHttpResponse {
    int status_code = 0;
    std::string body;
};

// Returns true once a response was received, whatever its status code --
// false is reserved for a transport-level failure (DNS, connect, TLS,
// timeout) that never produced an HTTP response at all, with `error`
// describing it. Mirrors _post_json's exception split in
// sidecar/planner.py: a non-2xx status is still "true" here and gets
// surfaced as a ChatSendResult error by SendMessages, exactly as an
// HTTPError there still carries a body to report.
using ChatHttpTransport =
    std::function<bool(const ChatHttpRequest& request,
                       ChatHttpResponse& response, std::string& error)>;

enum class ChatBlockType { Text, ToolUse, ToolResult };

// Provider-neutral content block. A given instance only ever populates the
// fields its `type` uses; the rest stay at their defaults.
struct ChatContentBlock {
    ChatBlockType type = ChatBlockType::Text;
    std::string text;         // Text; also ToolResult's textual content
    std::string tool_use_id;  // ToolUse's own id; the id a ToolResult answers
    std::string tool_name;    // ToolUse only
    mcp_json::Value tool_input;  // ToolUse only -- the tool call's arguments
    bool tool_is_error = false;  // ToolResult only

    static ChatContentBlock MakeText(std::string text);
    static ChatContentBlock MakeToolUse(std::string id, std::string name,
                                        mcp_json::Value input);
    static ChatContentBlock MakeToolResult(std::string toolUseId,
                                           std::string text, bool isError,
                                           std::string toolName = {});
};

// One turn of the conversation. `role` is "user" or "assistant", matching
// the Anthropic API verbatim -- this struct is a thin, ordered wrapper
// around one `messages[]` entry, not a richer model of its own.
struct ChatMessage {
    std::string role;
    std::vector<ChatContentBlock> content;
};

struct ChatLlmConfig {
    ChatLlmProvider provider = ChatLlmProvider::AnthropicMessages;
    std::string model = "claude-sonnet-4-5";
    std::string api_key;
    std::string base_url = "https://api.anthropic.com";
    std::string anthropic_version = "2023-06-01";
    int max_tokens = 4096;
    int context_tokens = 32768;
    double timeout_seconds = 120.0;

    // Reads ANTHROPIC_API_KEY, CUTMACHINE_ANTHROPIC_MODEL (falling back to
    // CUTMACHINE_MODEL, then the default above) and CUTMACHINE_ANTHROPIC_URL
    // from the process environment -- matching
    // sidecar/planner.py:AnthropicPlanner.__init__ field for field. Before
    // reading, also tries to fill in any of those three variables that
    // aren't already exported from a local dotenv file, in priority order:
    // $CUTMACHINE_ENV_FILE if set, else ~/.config/cutmachine/.env. That is
    // deliberately *not* the sidecar's project-root .env (PHILOSOPHY.md:
    // interface/tooling configuration is a local preference, never
    // something that lives in or next to the project) -- pointing
    // CUTMACHINE_ENV_FILE at the sidecar's own .env during development
    // reunifies the two if a developer wants that.
    //
    // Never overrides a variable already exported in the environment. If
    // ANTHROPIC_API_KEY is still unset afterward, api_key is left empty and
    // `missingKeyError` (if non-null) is set to an explanatory message --
    // this is the only "not configured" signal; the client never guesses or
    // silently disables itself otherwise.
    static ChatLlmConfig FromEnvironment(
        std::string* missingKeyError = nullptr);
};

struct ChatSendResult {
    bool ok = false;
    std::vector<ChatContentBlock> content;  // assistant's reply, if ok
    std::string stop_reason;                // "end_turn", "tool_use", ...
    std::string error;                      // set when !ok
};

class ChatLlmClient {
public:
    ChatLlmClient(ChatLlmConfig config, ChatHttpTransport transport);

    // Sends the full conversation so far plus the tool catalog and returns
    // the assistant's next turn. Stateless: `messages` already contains
    // every prior user/assistant/tool_result turn -- see ChatSession, which
    // owns that history and this loop.
    ChatSendResult SendMessages(const std::vector<ChatMessage>& messages,
                                const std::string& systemPrompt,
                                const std::vector<McpTool>& tools) const;

    const ChatLlmConfig& Config() const { return config_; }

private:
    ChatLlmConfig config_;
    ChatHttpTransport transport_;
};

}  // namespace chat

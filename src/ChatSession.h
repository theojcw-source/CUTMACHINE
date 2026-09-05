#pragma once

// The chat panel's tool-calling loop (ROADMAP.md F2.4): the piece that
// takes one user instruction, talks to the LLM, and dispatches whatever
// tool calls it asks for -- through McpToolRegistry::Call, the exact same
// entry point McpServer.cc's `tools/call` JSON-RPC handler uses. There is
// no second tool-execution path here: every mutation this class causes is
// `registry.Call(backend, name, arguments)`, i.e. McpTools.cc's dispatch
// for `name` constructing an Operation and handing it to `backend`, which
// for the running app (McpLiveBackend.h) means EditLog::Apply against the
// same in-memory Document a mouse gesture edits. See PHILOSOPHY.md
// principle 3: "un geste à la souris et une instruction en langage naturel
// produisent la même opération... passent par le même journal."
//
// Plain C++ (Json.h/McpTools.h/ChatLlmClient.h only) so the loop itself --
// message bookkeeping, when to stop asking for more tool calls, how a tool
// failure is fed back to the model -- is unit-testable on a plain Linux
// host against a fake ChatLlmClient transport and an in-memory McpBackend,
// without AppKit or a real Anthropic key. See tests/chat_session_tests.cc.

#include "ChatLlmClient.h"
#include "McpBackend.h"
#include "McpTools.h"

#include <functional>
#include <string>
#include <vector>

namespace chat {

enum class ChatEntryKind {
    UserMessage,    // the instruction that started this turn
    AssistantText,  // a text block the model produced
    ToolCall,    // a tool the model asked to run (before the result is known)
    ToolResult,  // that tool call's outcome
    Error,       // the turn stopped early: transport/protocol/safety-limit
};

// One line of the transcript the chat panel renders. Which fields are
// meaningful depends on `kind`, documented per field below.
struct ChatTranscriptEntry {
    ChatEntryKind kind = ChatEntryKind::UserMessage;
    std::string text;            // UserMessage/AssistantText/Error text;
                                 // ToolResult's success or refusal JSON
    std::string tool_name;       // ToolCall/ToolResult only
    std::string tool_args_json;  // ToolCall only, for display
    bool tool_ok = false;        // ToolResult only
};

// A single instruction's worth of default guidance, in French to match the
// rest of this project's UI copy (see PanelLayout.h's panel titles,
// sidecar/planner.py's own SYSTEM_PROMPT). Told to call the `describe` tool
// before proposing an edit rather than being handed a pre-fetched timeline
// view, so context-gathering goes through McpToolRegistry exactly like a
// mutation does -- see this file's header comment.
extern const char kDefaultSystemPrompt[];

class ChatSession {
public:
    // Neither `backend` nor `registry` nor `llm` is owned; all three must
    // outlive this session. `onEntry`, if set, is invoked synchronously
    // (on whatever thread calls SubmitUserMessage) as each transcript entry
    // is produced, so a caller can stream partial progress to the UI
    // instead of waiting for the whole turn; Transcript() always holds the
    // complete history regardless.
    ChatSession(
        McpBackend& backend, const McpToolRegistry& registry,
        const ChatLlmClient& llm,
        std::string systemPrompt = kDefaultSystemPrompt,
        std::function<void(const ChatTranscriptEntry&)> onEntry = nullptr);

    // Runs one full user turn: records the instruction, then alternates
    // LLM calls and tool dispatches until the model stops asking for tools,
    // it asks for more than kMaxToolIterations rounds in a row (a safety
    // limit against a runaway loop, not a normal outcome), or a call to the
    // LLM itself fails. Returns false only in the latter two cases --
    // `error` is also appended to Transcript() as a ChatEntryKind::Error
    // either way. Any tool calls that already ran (successfully or not)
    // before a stopping condition are recorded in Transcript() regardless
    // of the return value: a rejected tool call is not a turn failure, the
    // model is expected to see it and adjust.
    bool SubmitUserMessage(const std::string& text, std::string& error);

    const std::vector<ChatTranscriptEntry>& Transcript() const {
        return transcript_;
    }

private:
    void Emit(ChatTranscriptEntry entry);

    McpBackend& backend_;
    const McpToolRegistry& registry_;
    const ChatLlmClient& llm_;
    std::string system_prompt_;
    std::function<void(const ChatTranscriptEntry&)> on_entry_;
    std::vector<ChatMessage> messages_;
    std::vector<ChatTranscriptEntry> transcript_;
};

}  // namespace chat

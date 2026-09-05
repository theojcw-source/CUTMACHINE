// Pure-C++ test for the chat panel's tool-calling loop (ROADMAP.md F2.4).
// Depends only on Document/EditLog/Operations/Json/IdResolver/McpTools/
// ChatLlmClient/ChatSession -- no ProjectStorage, no AppKit -- so it builds
// and runs on a plain Linux host, the same way tests/mcp_tools_tests.cc
// does for the MCP server itself.
//
// The whole point of this file: prove that a tool call the (fake) LLM
// requests is dispatched through the *real* McpToolRegistry::Call -- the
// exact function McpServer.cc's `tools/call` JSON-RPC handler calls -- and
// so ends up applied via EditLog::Apply against a real in-memory Document,
// not some parallel chat-only mutation path. The in-memory backend below is
// structurally identical to tests/mcp_tools_tests.cc's InMemoryBackend for
// exactly that reason: it is the same kind of backend, just reused here to
// drive ChatSession instead of the JSON-RPC transport.

#include "ChatLlmClient.h"
#include "ChatSession.h"
#include "Document.h"
#include "EditLog.h"
#include "IdResolver.h"
#include "Json.h"
#include "McpBackend.h"
#include "McpTools.h"
#include "Operations.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using chat::ChatEntryKind;
using chat::ChatHttpRequest;
using chat::ChatHttpResponse;
using chat::ChatLlmClient;
using chat::ChatLlmConfig;
using chat::ChatSession;
using chat::ChatTranscriptEntry;
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

Document Fixture() {
    Document document;
    document.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000002",
         "video",
         0,
         {{"01K30000000000000000000003",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}}}},
    };
    return document;
}

// Same shape as tests/mcp_tools_tests.cc's InMemoryBackend: ApplyOperation/
// Undo/Redo call EditLog::Apply/Undo/Redo directly against a Document held
// in memory -- the same function ApplyOperationCommand's `--apply-op` path
// and McpLiveBackend (the app's real backend, McpLiveBackend.h) both call.
class InMemoryBackend : public McpBackend {
public:
    explicit InMemoryBackend(Document document, std::string describeJson = {})
        : document_(std::move(document)),
          describe_json_(std::move(describeJson)) {}

    bool SnapshotDocument(Document& document, std::string&) override {
        document = document_;
        return true;
    }

    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Apply(document_, std::move(operation), error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Undo(std::string& resultJson, std::string& errorName,
              std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Undo(document_, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Redo(std::string& resultJson, std::string& errorName,
              std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Redo(document_, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Describe(std::string& json, std::string&) override {
        if (!describe_json_.empty()) {
            json = describe_json_;
            return true;
        }
        json = "{\"clip_count\":" +
               std::to_string(document_.sequence.tracks.at(0).clips.size()) +
               "}";
        return true;
    }

    const Document& CurrentDocument() const { return document_; }

private:
    Document document_;
    EditLog log_;
    std::string describe_json_;
};

// A scripted transport: each call to SendMessages consumes the next
// pre-canned response body in `responses`. Lets a test drive a multi-turn
// tool-calling exchange (propose a tool call, see the result, respond)
// deterministically, without a real model or network.
class ScriptedTransport {
public:
    explicit ScriptedTransport(std::vector<std::string> responses)
        : responses_(std::move(responses)) {}

    bool operator()(const ChatHttpRequest& request, ChatHttpResponse& response,
                    std::string& error) {
        if (call_count_ >= responses_.size()) {
            error = "scripted transport ran out of responses";
            return false;
        }
        response.status_code = 200;
        response.body = responses_[call_count_];
        requests_.push_back(request);
        ++call_count_;
        return true;
    }

    size_t CallCount() const { return call_count_; }
    const ChatHttpRequest& Request(size_t index) const {
        return requests_.at(index);
    }

private:
    std::vector<std::string> responses_;
    std::vector<ChatHttpRequest> requests_;
    size_t call_count_ = 0;
};

ChatLlmConfig TestConfig() {
    ChatLlmConfig config;
    config.api_key = "sk-test-key";
    return config;
}

ChatLlmConfig OllamaConfig() {
    ChatLlmConfig config;
    config.provider = chat::ChatLlmProvider::Ollama;
    config.model = "qwen2.5-coder:7b";
    config.base_url = "http://localhost:11434";
    return config;
}

}  // namespace

int main() {
    Test(
        "a single tool_use turn dispatches through McpToolRegistry and "
        "mutates the real Document via EditLog::Apply",
        [] {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            ScriptedTransport transport({
                R"({"content":[)"
                R"({"type":"text","text":"Je raccourcis le plan."},)"
                R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
                R"("input":{"clip_id":"01K30000000000000000000003",)"
                R"("edge":"Tail","delta":{"value":-1,"rate":25}}})"
                R"(],"stop_reason":"tool_use"})",
                R"({"content":[{"type":"text","text":"C'est fait."}],)"
                R"("stop_reason":"end_turn"})",
            });
            ChatLlmClient llm(TestConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok = session.SubmitUserMessage(
                "Raccourcis le premier plan d'une image.", error);
            Check(ok, "the turn succeeds: " + error);
            Check(transport.CallCount() == 2,
                  "two LLM round trips: propose the tool, then confirm");

            const Document& document = backend.CurrentDocument();
            Check(
                document.sequence.tracks.at(0).clips.at(0).duration.value == 9,
                "the clip was actually trimmed (duration 10 -> 9 frames) "
                "through EditLog::Apply, not a parallel mutation path");

            const std::vector<ChatTranscriptEntry>& transcript =
                session.Transcript();
            bool sawToolCall = false;
            bool sawSuccessfulResult = false;
            for (const ChatTranscriptEntry& entry : transcript) {
                if (entry.kind == ChatEntryKind::ToolCall &&
                    entry.tool_name == "trim_clip")
                    sawToolCall = true;
                if (entry.kind == ChatEntryKind::ToolResult &&
                    entry.tool_name == "trim_clip" && entry.tool_ok)
                    sawSuccessfulResult = true;
            }
            Check(sawToolCall,
                  "the transcript records the trim_clip tool call");
            Check(sawSuccessfulResult,
                  "the transcript records a successful trim_clip result");
        });

    Test(
        "a text-only reply with no tool_use ends the turn after one round trip",
        [] {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            ScriptedTransport transport({
                R"({"content":[{"type":"text","text":"Je ne peux pas faire cela."}],)"
                R"("stop_reason":"end_turn"})",
            });
            ChatLlmClient llm(TestConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok = session.SubmitUserMessage(
                "Fais quelque chose d'impossible.", error);
            Check(ok, "the turn succeeds: " + error);
            Check(transport.CallCount() == 1, "exactly one LLM round trip");
            Check(backend.CurrentDocument()
                          .sequence.tracks.at(0)
                          .clips.at(0)
                          .duration.value == 10,
                  "the document is untouched when no tool is called");
        });

    Test(
        "a failed tool call is reported in the transcript, fed back as an "
        "error tool_result, and does not mutate the document",
        [] {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            // Same rejection McpTools.cc's ID resolver reports to any MCP
            // client for an unresolvable clip_id -- see IdResolver::Resolve.
            // Caught before an Operation is even constructed, so this also
            // proves a bad chat-driven call never reaches EditLog::Apply.
            ScriptedTransport transport({
                R"({"content":[)"
                R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
                R"("input":{"clip_id":"01K39999999999999999999999",)"
                R"("edge":"Tail","delta":{"value":-1,"rate":25}}})"
                R"(],"stop_reason":"tool_use"})",
                R"({"content":[{"type":"text","text":"Ce clip n'existe pas."}],)"
                R"("stop_reason":"end_turn"})",
            });
            ChatLlmClient llm(TestConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok =
                session.SubmitUserMessage("Raccourcis un clip inconnu.", error);
            Check(ok, "the turn still succeeds overall: " + error);
            Check(backend.CurrentDocument()
                          .sequence.tracks.at(0)
                          .clips.at(0)
                          .duration.value == 10,
                  "the rejected tool call left the document untouched");

            bool sawFailedResult = false;
            for (const ChatTranscriptEntry& entry : session.Transcript()) {
                if (entry.kind == ChatEntryKind::ToolResult && !entry.tool_ok) {
                    sawFailedResult = true;
                    Check(entry.text.find("ValidationFailed") !=
                                  std::string::npos &&
                              entry.text.find("no object matches") !=
                                  std::string::npos,
                          "the failure names the resolver's own rejection, "
                          "got: " +
                              entry.text);
                }
            }
            Check(sawFailedResult,
                  "the transcript records the failed tool call");
        });

    Test(
        "Ollama describe results are compacted before the model follow-up", [] {
            const std::string description =
                R"({"sequence":{"name":"Montage"},"timeline":{)"
                R"("duration":{"frames":10},"tracks":[{"items":[)"
                R"({"type":"clip","source_id":"used"}]}],"sources":[)"
                R"({"id":"used","file":"used.mov"},)"
                R"({"id":"unused","file":"unused.mov"}]},"library":[)"
                R"({"alias":"M1","id":"unused","filename":"unused.mov",)"
                R"("codec":"codec_marker","in_use":false}],)"
                R"("bins":[],"markers":[]})";
            InMemoryBackend backend(Fixture(), description);
            McpToolRegistry registry;
            ScriptedTransport transport({
                R"({"message":{"role":"assistant","content":)"
                R"("{\"name\":\"describe\",\"arguments\":{}}"},)"
                R"("done":true,"done_reason":"stop"})",
                R"({"message":{"role":"assistant","content":"Diagnostic."},)"
                R"("done":true,"done_reason":"stop"})",
            });
            ChatLlmClient llm(OllamaConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok = session.SubmitUserMessage("Décris.", error);
            Check(ok, "the Ollama describe loop succeeds: " + error);
            Check(transport.CallCount() == 2,
                  "textual describe call produces a model follow-up");

            Value request;
            std::string parseError;
            Check(Value::Parse(transport.Request(1).body, request, parseError),
                  "follow-up request is valid JSON: " + parseError);
            const Value* messages = request.Find("messages");
            const Value* content = nullptr;
            if (messages && messages->IsArray() && !messages->AsArray().empty())
                content = messages->AsArray().back().Find("content");
            Value compact;
            Check(content && content->IsString() &&
                      Value::Parse(content->AsString(), compact, parseError),
                  "tool content remains structured JSON: " + parseError);
            const Value* timeline = compact.Find("timeline");
            const Value* sources =
                timeline ? timeline->Find("sources") : nullptr;
            Check(
                sources && sources->IsArray() && sources->AsArray().size() == 1,
                "only timeline-used sources remain in the compact view");
            const Value* library = compact.Find("library");
            Check(library && library->IsArray() &&
                      library->AsArray().front().Find("codec") == nullptr,
                  "heavy library metadata is omitted for the local model");
        });

    Test("a transport failure stops the turn and is recorded as an error", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse&,
                            std::string& transportError) {
            transportError = "connection refused";
            return false;
        };
        ChatLlmClient llm(TestConfig(), transport);
        ChatSession session(backend, registry, llm);

        std::string error;
        const bool ok = session.SubmitUserMessage("Peu importe.", error);
        Check(!ok, "the turn is reported as failed");
        Check(error.find("connection refused") != std::string::npos,
              "the transport error is surfaced, got: " + error);
        Check(session.Transcript().back().kind == ChatEntryKind::Error,
              "the transcript ends with an Error entry");
    });

    Test("a model that keeps requesting tools forever trips the safety limit", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        std::vector<std::string> responses;
        for (int i = 0; i < 20; ++i) {
            responses.push_back(
                R"({"content":[{"type":"tool_use","id":"toolu_)" +
                std::to_string(i) +
                R"(","name":"describe","input":{}}],"stop_reason":"tool_use"})");
        }
        ScriptedTransport transport(std::move(responses));
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);

        std::string error;
        const bool ok = session.SubmitUserMessage("Boucle infinie.", error);
        Check(!ok, "the turn is reported as failed once the limit trips");
        Check(!error.empty(), "an explanatory error is set");
        Check(transport.CallCount() < 20,
              "the loop actually stopped instead of exhausting the script");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All chat session tests passed\n";
    return 0;
}

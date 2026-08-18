// Pure-C++ subset test for the MCP server (ROADMAP.md F1.1/F1.2). Depends
// only on Document/EditLog/Operations/Json/IdResolver/McpTools/HttpServer/
// McpServer -- no ProjectStorage (CommonCrypto), no FFmpeg, no AppKit/Metal
// -- so it builds and runs on a plain Linux host, the same way
// tests/model_tests.cc and tests/edit_tests.cc do.
//
// It exercises the real HTTP + JSON-RPC transport (McpServer/HttpServer)
// end to end against an in-memory McpBackend that calls EditLog::Apply/
// Undo/Redo directly -- the exact function ApplyOperationCommand's
// `--apply-op` path calls, just without the project-file round trip. See
// tests/mcp_tests.cc for the macOS-only counterpart that goes through the
// real project file and ApplyOperationCommand byte for byte.

#include "Document.h"
#include "EditLog.h"
#include "IdResolver.h"
#include "Json.h"
#include "McpBackend.h"
#include "McpServer.h"
#include "Operations.h"
#include "Ulid.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
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
           {5, 25}},
          {"01K30000000000000000000004",
           "01K30000000000000000000001",
           {200, 25},
           {10, 25},
           {20, 25}}}},
    };
    return document;
}

// In-memory McpBackend for testing: no project file, no ProjectStorage.
// ApplyOperation/Undo/Redo call EditLog::Apply/Undo/Redo directly against a
// Document held in memory -- the same function ApplyOperationCommand's
// `--apply-op` path calls.
class InMemoryBackend : public McpBackend {
public:
    explicit InMemoryBackend(Document document)
        : document_(std::move(document)) {}

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
        size_t clipCount = 0;
        for (const DocumentTrack& track : document_.sequence.tracks)
            clipCount += track.clips.size();
        json = "{\"clip_count\":" + std::to_string(clipCount) + "}";
        return true;
    }

    const Document& CurrentDocument() const { return document_; }
    EditLog& Log() { return log_; }

private:
    Document document_;
    EditLog log_;
};

// Minimal blocking HTTP client: sends one POST and reads the response until
// the peer closes the connection (HttpServer always responds with
// `Connection: close`).
std::string HttpPostJson(int port, const std::string& path,
                         const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        throw std::runtime_error("connect() failed");
    }
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    const std::string requestText = request.str();
    size_t sent = 0;
    while (sent < requestText.size()) {
        const ssize_t wrote =
            ::send(fd, requestText.data() + sent, requestText.size() - sent, 0);
        if (wrote <= 0) break;
        sent += static_cast<size_t>(wrote);
    }
    std::string response;
    char chunk[4096];
    ssize_t got;
    while ((got = ::recv(fd, chunk, sizeof(chunk), 0)) > 0)
        response.append(chunk, static_cast<size_t>(got));
    ::close(fd);
    return response;
}

std::string StatusLine(const std::string& httpResponse) {
    return httpResponse.substr(0, httpResponse.find("\r\n"));
}

std::string HttpBody(const std::string& httpResponse) {
    const size_t headerEnd = httpResponse.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return "";
    return httpResponse.substr(headerEnd + 4);
}

}  // namespace

int main() {
    // Built once: Document's default sequence ID is itself a fresh
    // GenerateUlid() on every construction, so two independent Fixture()
    // calls would legitimately disagree on sequence.id alone. Every
    // "expected" comparison below copies this same fixture rather than
    // calling Fixture() again.
    const Document fixture = Fixture();
    InMemoryBackend backend(fixture);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError),
          "MCP server starts on an ephemeral port: " + startError);
    Check(server.Port() != 0, "server reports its bound port");

    // ---- tools/list ----
    const std::string listRequest =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    const std::string listResponse =
        HttpPostJson(server.Port(), "/mcp", listRequest);
    Check(StatusLine(listResponse) == "HTTP/1.1 200 OK",
          "tools/list returns HTTP 200");
    mcp_json::Value listBody;
    std::string parseError;
    Check(mcp_json::Value::Parse(HttpBody(listResponse), listBody, parseError),
          "tools/list body is valid JSON: " + parseError);
    const mcp_json::Value* toolsField =
        listBody.Find("result") ? listBody.Find("result")->Find("tools")
                                : nullptr;
    Check(toolsField && toolsField->IsArray() &&
              toolsField->AsArray().size() > 30,
          "tools/list exposes the full catalog (>30 tools)");
    bool sawInsertClip = false;
    bool sawClearClips = false;
    bool sawTrimClip = false;
    bool sawUndo = false;
    if (toolsField) {
        for (const mcp_json::Value& tool : toolsField->AsArray()) {
            const mcp_json::Value* name = tool.Find("name");
            if (!name) continue;
            if (name->AsString() == "insert_clip") sawInsertClip = true;
            if (name->AsString() == "clear_clips") sawClearClips = true;
            if (name->AsString() == "trim_clip") sawTrimClip = true;
            if (name->AsString() == "undo") sawUndo = true;
            // Multicam operations are stubbed pending F1.5; must not be
            // offered as a working tool.
            Check(name->AsString() != "add_multicam_group" &&
                      name->AsString() != "set_multicam_active_angle",
                  "tools/list excludes stubbed multicam operations");
        }
    }
    Check(sawInsertClip, "tools/list includes insert_clip");
    Check(sawClearClips, "tools/list includes clear_clips");
    Check(sawTrimClip, "tools/list includes trim_clip");
    Check(sawUndo, "tools/list includes undo");

    // ---- tools/call: trim_clip, compared against a direct EditLog::Apply ----
    // Exercise the ID resolver: "01K3000000000000000000000" + "3" is a full
    // ID; a short unambiguous prefix should resolve identically.
    const std::string trimRequest =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K3000000000000000000000)"
        R"(3","edge":"Tail","delta":{"value":-1,"rate":25}}}})";
    const std::string trimResponse =
        HttpPostJson(server.Port(), "/mcp", trimRequest);
    mcp_json::Value trimBody;
    Check(mcp_json::Value::Parse(HttpBody(trimResponse), trimBody, parseError),
          "tools/call trim_clip body is valid JSON: " + parseError);
    const mcp_json::Value* trimResult = trimBody.Find("result");
    Check(trimResult != nullptr, "tools/call trim_clip returns a result");
    if (trimResult) {
        const mcp_json::Value* isError = trimResult->Find("isError");
        Check(isError && isError->IsBool() && isError->AsBool() == false,
              "tools/call trim_clip is not an error");
    }

    Document expected = fixture;
    EditLog expectedLog;
    EditError expectedError = EditError::None;
    std::string expectedMessage;
    const Operation expectedTrim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    Check(expectedLog.Apply(expected, expectedTrim, expectedError,
                            expectedMessage),
          "reference direct EditLog::Apply(trim) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "tools/call trim_clip changes the document exactly as a direct "
          "EditLog::Apply/--apply-op call would");

    // ---- tools/call: arbitrary atomic multi-clear ----
    const std::string clearRequest =
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call",)"
        R"("params":{"name":"clear_clips","arguments":{"clip_ids":[)"
        R"("01K30000000000000000000003",)"
        R"("01K30000000000000000000004"]}}})";
    const std::string clearResponse =
        HttpPostJson(server.Port(), "/mcp", clearRequest);
    Check(clearResponse.find("\"isError\":false") != std::string::npos,
          "tools/call clear_clips is not an error");
    Check(expectedLog.Apply(expected,
                            ClearClipsOperation{{"01K30000000000000000000003",
                                                 "01K30000000000000000000004"},
                                                {}},
                            expectedError, expectedMessage),
          "reference direct EditLog::Apply(clear_clips) succeeds: " +
              expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "tools/call clear_clips matches a direct atomic operation");

    // ---- tools/call: insert_clip ----
    // insert_clip's clip_id is engine-generated (ApplyInsert's own
    // GenerateUlid(), same as a human/CLI insert never supplying one), so
    // the MCP call and an independently-constructed reference operation
    // cannot agree on it by chance. Discover the ID the server actually
    // assigned, then replay a reference InsertClipOperation with that exact
    // ID for a true byte-for-byte comparison -- this is the same reasoning
    // ApplyInsert itself documents: the ID is retained "for redo/inverse
    // identity", not invented independently on each replay.
    std::vector<Ulid> clipIdsBeforeInsert;
    for (const DocumentTrack& track : backend.CurrentDocument().sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            clipIdsBeforeInsert.push_back(clip.id);
    const std::string insertRequest =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
        R"("params":{"name":"insert_clip","arguments":{)"
        R"("track_id":"01K30000000000000000000002",)"
        R"("source_id":"01K30000000000000000000001",)"
        R"("source_in":{"value":0,"rate":25},)"
        R"("duration":{"value":5,"rate":25},)"
        R"("timeline_in":{"value":0,"rate":25}}}})";
    const std::string insertResponse =
        HttpPostJson(server.Port(), "/mcp", insertRequest);
    Check(insertResponse.find("\"isError\":false") != std::string::npos,
          "tools/call insert_clip is not an error");

    Ulid insertedClipId;
    for (const DocumentTrack& track :
         backend.CurrentDocument().sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (std::find(clipIdsBeforeInsert.begin(),
                          clipIdsBeforeInsert.end(),
                          clip.id) == clipIdsBeforeInsert.end())
                insertedClipId = clip.id;
        }
    }
    Check(!insertedClipId.empty(),
          "insert_clip created exactly one new, previously-unseen clip_id");

    Operation expectedInsert = InsertClipOperation{"01K30000000000000000000002",
                                                   "01K30000000000000000000001",
                                                   {0, 25},
                                                   {5, 25},
                                                   {0, 25},
                                                   insertedClipId,
                                                   {}};
    Check(
        expectedLog.Apply(expected, expectedInsert, expectedError,
                          expectedMessage),
        "reference direct EditLog::Apply(insert) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "tools/call insert_clip changes the document exactly as a direct "
          "EditLog::Apply/--apply-op call would");

    // ---- undo/redo tools ----
    const std::string undoRequest =
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"undo","arguments":{}}})";
    HttpPostJson(server.Port(), "/mcp", undoRequest);
    EditError undoError = EditError::None;
    std::string undoMessage;
    Check(expectedLog.Undo(expected, undoError, undoMessage),
          "reference direct EditLog::Undo succeeds: " + undoMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "the undo tool matches a direct EditLog::Undo call");

    // ---- error path: unknown clip leaves the document untouched ----
    const std::string beforeError = backend.CurrentDocument().SaveToString();
    const std::string badRequest =
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K39999999999999999999999","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25}}}})";
    const std::string badResponse =
        HttpPostJson(server.Port(), "/mcp", badRequest);
    mcp_json::Value badBody;
    Check(mcp_json::Value::Parse(HttpBody(badResponse), badBody, parseError),
          "error response body is valid JSON: " + parseError);
    const mcp_json::Value* badResult = badBody.Find("result");
    bool badIsError = false;
    if (badResult) {
        const mcp_json::Value* isError = badResult->Find("isError");
        badIsError = isError && isError->IsBool() && isError->AsBool();
    }
    Check(badIsError,
          "an unknown clip_id is reported as isError, not a "
          "JSON-RPC protocol error");
    Check(backend.CurrentDocument().SaveToString() == beforeError,
          "a refused tool call leaves the document byte-identical");

    // ---- argument validation: unknown key is rejected by name ----
    const std::string unknownKeyRequest =
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25},"bogus":true}}})";
    const std::string unknownKeyResponse =
        HttpPostJson(server.Port(), "/mcp", unknownKeyRequest);
    Check(unknownKeyResponse.find("unknown argument 'bogus'") !=
              std::string::npos,
          "an unknown argument is rejected and named in the error");
    Check(backend.CurrentDocument().SaveToString() == beforeError,
          "an argument-validation failure leaves the document byte-identical");

    // ---- ID resolution: ambiguous prefix is refused, never guessed ----
    // Every fixture ID shares the "01K3000000000000000000000" prefix, so
    // that exact string is ambiguous among clip/track/source IDs.
    const std::string ambiguousRequest =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K3000000000000000000000","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25}}}})";
    const std::string ambiguousResponse =
        HttpPostJson(server.Port(), "/mcp", ambiguousRequest);
    Check(ambiguousResponse.find("ambiguous") != std::string::npos,
          "an ambiguous id prefix is refused rather than silently guessed");

    // ---- non-finite number rejected ----
    const std::string hugeExponentRequest =
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":1e400,"rate":25}}}})";
    const std::string hugeExponentResponse =
        HttpPostJson(server.Port(), "/mcp", hugeExponentRequest);
    Check(hugeExponentResponse.find("Parse error") != std::string::npos,
          "a non-finite number literal is rejected");

    // ---- initialize ----
    const std::string initRequest =
        R"({"jsonrpc":"2.0","id":9,"method":"initialize",)"
        R"("params":{"protocolVersion":"2024-11-05","capabilities":{},)"
        R"("clientInfo":{"name":"test","version":"0"}}})";
    const std::string initResponse =
        HttpPostJson(server.Port(), "/mcp", initRequest);
    Check(initResponse.find("\"protocolVersion\":\"2024-11-05\"") !=
              std::string::npos,
          "initialize echoes the negotiated protocol version");
    Check(initResponse.find("\"serverInfo\"") != std::string::npos,
          "initialize reports serverInfo");

    // ---- JSON-RPC notification: no response body over HTTP ----
    const std::string notificationRequest =
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    const std::string notificationResponse =
        HttpPostJson(server.Port(), "/mcp", notificationRequest);
    Check(StatusLine(notificationResponse) == "HTTP/1.1 202 Accepted",
          "a JSON-RPC notification gets HTTP 202 with no JSON-RPC response");
    Check(HttpBody(notificationResponse).empty(),
          "a JSON-RPC notification gets an empty body");

    // ---- unknown method is a JSON-RPC protocol error ----
    const std::string unknownMethodRequest =
        R"({"jsonrpc":"2.0","id":10,"method":"not/a/real/method"})";
    const std::string unknownMethodResponse =
        HttpPostJson(server.Port(), "/mcp", unknownMethodRequest);
    Check(unknownMethodResponse.find("-32601") != std::string::npos,
          "an unknown JSON-RPC method reports 'Method not found'");

    server.Stop();
    Check(!server.IsRunning(), "server reports stopped after Stop()");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All MCP tool tests passed\n";
    return 0;
}

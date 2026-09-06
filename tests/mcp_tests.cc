// Integration test for the MCP server against the real, on-disk project
// backend (ROADMAP.md F1.1/F1.2): starts an McpServer bound to a
// McpProjectBackend over a real portable project package, drives it purely
// over HTTP + JSON-RPC, and asserts the resulting project file is
// byte-identical to what a direct ApplyOperationCommand/`--apply-op` call
// on a duplicate fixture would produce.
//
// McpProjectBackend depends on ProjectStorage, which -- like the rest of
// the project-facing CLI surface tested in tests/cli_tests.cc -- needs
// CommonCrypto and therefore only builds on macOS. See
// tests/mcp_tools_tests.cc for the pure-C++ counterpart that exercises the
// same HTTP/JSON-RPC/tool-dispatch/ID-resolver code in a plain Linux build,
// against an in-memory backend instead of a project file.

#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "IdResolver.h"
#include "Json.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

Document Fixture() {
    Document document;
    // Pinned, not left to default to a fresh GenerateUlid(): the assertions
    // below compare two independently-built copies of this fixture
    // byte-for-byte after the same edit, so every ID the serializer writes
    // has to be fixed up front or the comparison can never hold.
    document.sequence.id = "01K30000000000000000000005";
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

std::string HttpBody(const std::string& httpResponse) {
    const size_t headerEnd = httpResponse.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return "";
    return httpResponse.substr(headerEnd + 4);
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-mcp-tests");
    std::filesystem::create_directory(directory);
    std::string error;

    // Two independent copies of the same fixture: one the MCP server edits
    // over HTTP, one a reference edited directly through ApplyOperationCommand
    // (the exact function `--apply-op` calls).
    Project mcpProject = Project::FromDocument(Fixture(), "MCP fixture");
    const Ulid alternateTimelineId = "01K30000000000000000000006";
    DocumentSequence alternateTimeline = mcpProject.timelines.front();
    alternateTimeline.id = alternateTimelineId;
    alternateTimeline.name = "Alternate";
    alternateTimeline.tracks[0].id = "01K30000000000000000000007";
    alternateTimeline.tracks[0].clips[0].id = "01K30000000000000000000008";
    alternateTimeline.tracks[0].clips[1].id = "01K30000000000000000000009";
    mcpProject.timelines.push_back(alternateTimeline);
    std::string mcpPath;
    Check(CreatePortableProject((directory / "Mcp.cutmachine-project").string(),
                                mcpProject, mcpPath, error),
          "MCP fixture package saves: " + error);

    Project referenceProject =
        Project::FromDocument(Fixture(), "Reference fixture");
    DocumentSequence referenceAlternate = referenceProject.timelines.front();
    referenceAlternate.id = alternateTimelineId;
    referenceAlternate.name = "Alternate";
    referenceAlternate.tracks[0].id = "01K30000000000000000000007";
    referenceAlternate.tracks[0].clips[0].id = "01K30000000000000000000008";
    referenceAlternate.tracks[0].clips[1].id = "01K30000000000000000000009";
    referenceProject.timelines.push_back(referenceAlternate);
    std::string referencePath;
    Check(CreatePortableProject(
              (directory / "Reference.cutmachine-project").string(),
              referenceProject, referencePath, error),
          "reference fixture package saves: " + error);

    McpProjectBackend backend(mcpPath);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError),
          "MCP server starts on an ephemeral port: " + startError);

    // ---- tools/list exposes the full catalog ----
    const std::string listResponse =
        HttpPostJson(server.Port(), "/mcp",
                     R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    mcp_json::Value listBody;
    std::string parseError;
    Check(mcp_json::Value::Parse(HttpBody(listResponse), listBody, parseError),
          "tools/list body is valid JSON: " + parseError);
    const mcp_json::Value* toolsField =
        listBody.Find("result") ? listBody.Find("result")->Find("tools")
                                : nullptr;
    Check(toolsField && toolsField->IsArray() &&
              toolsField->AsArray().size() > 30,
          "tools/list exposes the full catalog (>30 tools) over the real "
          "project-file backend");

    // ---- tools/call trim_clip vs. a direct --apply-op call ----
    const std::string trimRequest =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":-1,"rate":25}}}})";
    const std::string trimResponse =
        HttpPostJson(server.Port(), "/mcp", trimRequest);
    Check(trimResponse.find("\"isError\":false") != std::string::npos,
          "tools/call trim_clip is not an error");

    const Operation referenceTrim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    std::string applyOpOutput;
    Check(
        ApplyOperationCommand(referencePath, SerializeOperation(referenceTrim),
                              applyOpOutput) == 0,
        "reference --apply-op trim succeeds: " + applyOpOutput);

    Project mcpAfterTrim;
    Project referenceAfterTrim;
    Check(Project::Load(mcpPath, mcpAfterTrim, error),
          "MCP-edited project reloads after trim: " + error);
    Check(Project::Load(referencePath, referenceAfterTrim, error),
          "reference project reloads after trim: " + error);
    Check(mcpAfterTrim.MakeActiveDocument().SaveToString() ==
              referenceAfterTrim.MakeActiveDocument().SaveToString(),
          "tools/call trim_clip changes the on-disk document exactly as a "
          "direct --apply-op call would");

    // ---- tools/call insert_clip vs. a direct --apply-op call ----
    // insert_clip's clip_id is engine-generated; discover the ID the MCP
    // call actually produced and replay it explicitly against the
    // reference project for a true byte-for-byte comparison (see
    // tests/mcp_tools_tests.cc for the same reasoning in more detail).
    std::vector<Ulid> clipIdsBeforeInsert;
    for (const DocumentTrack& track :
         mcpAfterTrim.MakeActiveDocument().sequence.tracks)
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

    Project mcpAfterInsert;
    Check(Project::Load(mcpPath, mcpAfterInsert, error),
          "MCP-edited project reloads after insert: " + error);
    Ulid insertedClipId;
    for (const DocumentTrack& track :
         mcpAfterInsert.MakeActiveDocument().sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (std::find(clipIdsBeforeInsert.begin(),
                          clipIdsBeforeInsert.end(),
                          clip.id) == clipIdsBeforeInsert.end())
                insertedClipId = clip.id;
        }
    }
    Check(!insertedClipId.empty(),
          "insert_clip created exactly one new, previously-unseen clip_id");

    const Operation referenceInsert =
        InsertClipOperation{"01K30000000000000000000002",
                            "01K30000000000000000000001",
                            {0, 25},
                            {5, 25},
                            {0, 25},
                            insertedClipId,
                            {}};
    Check(ApplyOperationCommand(referencePath,
                                SerializeOperation(referenceInsert),
                                applyOpOutput) == 0,
          "reference --apply-op insert succeeds: " + applyOpOutput);
    Project referenceAfterInsert;
    Check(Project::Load(referencePath, referenceAfterInsert, error),
          "reference project reloads after insert: " + error);
    Check(mcpAfterInsert.MakeActiveDocument().SaveToString() ==
              referenceAfterInsert.MakeActiveDocument().SaveToString(),
          "tools/call insert_clip changes the on-disk document exactly as a "
          "direct --apply-op call would");

    // ---- undo tool vs. a direct undo through the same timeline EditLog ----
    const std::string undoRequest =
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"undo","arguments":{}}})";
    const std::string undoResponse =
        HttpPostJson(server.Port(), "/mcp", undoRequest);
    Check(undoResponse.find("\"isError\":false") != std::string::npos,
          "tools/call undo is not an error");
    Check(UndoOperationCommand(referencePath, applyOpOutput) == 0,
          "reference undo-op succeeds: " + applyOpOutput);
    Project mcpAfterUndo;
    Project referenceAfterUndo;
    Check(Project::Load(mcpPath, mcpAfterUndo, error),
          "MCP-edited project reloads after undo: " + error);
    Check(Project::Load(referencePath, referenceAfterUndo, error),
          "reference project reloads after undo: " + error);
    Check(mcpAfterUndo.MakeActiveDocument().SaveToString() ==
              referenceAfterUndo.MakeActiveDocument().SaveToString(),
          "the undo tool matches a direct timeline undo");
    Check(mcpAfterUndo.MakeActiveDocument().SaveToString() ==
              mcpAfterTrim.MakeActiveDocument().SaveToString(),
          "undoing the insert restores the exact post-trim document");

    // ---- B9: an explicit non-active timeline is the sole edit target ----
    const std::string activeBeforeExplicit =
        mcpAfterUndo.MakeActiveDocument().SaveToString();
    const Document alternateBeforeExplicit =
        mcpAfterUndo.MakeDocument(alternateTimelineId);
    const std::string explicitTrimRequest =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"timeline_id":"01K30000000000000000000006",)"
        R"("clip_id":"01K30000000000000000000008","edge":"Tail",)"
        R"("delta":{"value":-1,"rate":25}}}})";
    const std::string explicitTrimResponse =
        HttpPostJson(server.Port(), "/mcp", explicitTrimRequest);
    Check(explicitTrimResponse.find("\"isError\":false") != std::string::npos,
          "an MCP edit accepts an explicit non-active timeline_id");
    Project afterExplicitTrim;
    Check(Project::Load(mcpPath, afterExplicitTrim, error),
          "project reloads after an explicit-timeline edit: " + error);
    Check(afterExplicitTrim.active_timeline_id ==
                  mcpAfterUndo.active_timeline_id &&
              afterExplicitTrim.MakeActiveDocument().SaveToString() ==
                  activeBeforeExplicit,
          "an explicit MCP edit preserves the active timeline byte-for-byte");
    const Document alternateAfterExplicit =
        afterExplicitTrim.MakeDocument(alternateTimelineId);
    const DocumentClip* alternateBeforeClip =
        alternateBeforeExplicit.FindClip("01K30000000000000000000008");
    const DocumentClip* alternateAfterClip =
        alternateAfterExplicit.FindClip("01K30000000000000000000008");
    Check(alternateBeforeClip && alternateAfterClip &&
              alternateAfterClip->duration == RationalTime{9, 25} &&
              alternateBeforeClip->duration == RationalTime{10, 25},
          "the explicit non-active timeline receives the edit");

    // ---- error path leaves the project file untouched ----
    const std::string beforeError = Read(mcpPath);
    const std::string badRequest =
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K39999999999999999999999","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25}}}})";
    const std::string badResponse =
        HttpPostJson(server.Port(), "/mcp", badRequest);
    Check(badResponse.find("\"isError\":true") != std::string::npos,
          "an unknown clip_id is reported as isError, not a JSON-RPC "
          "protocol error");
    // An ID that resolves to nothing never reaches the engine: IdResolver
    // rejects it first, so the reported error is ValidationFailed naming the
    // offending argument, not the engine's UnknownClip. That is the more
    // precise of the two -- it tells the agent *which* argument was wrong --
    // so the resolver deliberately owns this case.
    Check(badResponse.find("ValidationFailed") != std::string::npos &&
              badResponse.find("trim_clip.clip_id") != std::string::npos,
          "an unresolvable id is refused by the resolver, naming the "
          "offending argument");
    Check(Read(mcpPath) == beforeError,
          "a refused tool call leaves the project file byte-identical");

    // ...but an operation the resolver accepts and the *engine* refuses
    // still reports the engine's own EditError name verbatim, which is what
    // keeps MCP error reporting indistinguishable from `--apply-op`. The
    // clip_id here is real, so resolution succeeds and the refusal comes
    // from ApplyOperation: trimming far more off the tail than the clip has
    // left would drive its duration to zero or below.
    const std::string badTrimRequest =
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":-500,"rate":25}}}})";
    const std::string badTrimResponse =
        HttpPostJson(server.Port(), "/mcp", badTrimRequest);
    Check(badTrimResponse.find("InvalidDuration") != std::string::npos,
          "an engine-level refusal names the exact EditError, matching "
          "--apply-op");
    Check(Read(mcpPath) == beforeError,
          "an engine-refused tool call also leaves the project file "
          "byte-identical");

    // ---- the session cache never outlives an edit made through it ----
    // PERF-2026-09. McpProjectBackend keeps the project it loaded for the
    // life of the session, because one tool call used to reload the whole
    // package three or four times over. What makes that sound is that every
    // command drops the cache -- not the size/mtime stamp kept beside it,
    // which is only a second opinion for a package edited outside
    // CUTMACHINE. So this drives an edit the stamp cannot see: one that
    // leaves the project file exactly as long, with its modification time
    // put back. That is not a contrivance -- it is what an exFAT volume,
    // whose timestamps advance in two-second steps, produces on its own, and
    // the LaCie drives this project is edited from are exFAT.
    Project cacheProject = Project::FromDocument(Fixture(), "Cache fixture");
    std::string cachePath;
    Check(CreatePortableProject(
              (directory / "Cache.cutmachine-project").string(), cacheProject,
              cachePath, error),
          "cache fixture package saves: " + error);

    McpProjectBackend cacheBackend(cachePath);
    Document cachedBefore;
    std::string cacheMessage;
    Check(cacheBackend.SnapshotDocument(cachedBefore, cacheMessage),
          "the first snapshot loads and caches the project: " + cacheMessage);

    const std::filesystem::path cacheFile(cachePath);
    const auto stampedTime = std::filesystem::last_write_time(cacheFile);
    const auto stampedSize = std::filesystem::file_size(cacheFile);

    // timeline_in 5 -> 9: one digit for one digit, so the serialized project
    // keeps its exact length. The clip occupies [9,19), still clear of the
    // second clip at [20,30).
    std::string moveResult;
    std::string moveErrorName;
    Check(cacheBackend.ApplyOperation(
              MoveClipOperation{"01K30000000000000000000003",
                                "01K30000000000000000000002",
                                {9, 25},
                                {}},
              moveResult, moveErrorName, cacheMessage),
          "the edit through the cached backend applies: " + moveErrorName +
              " " + cacheMessage);
    Check(std::filesystem::file_size(cacheFile) == stampedSize,
          "the fixture edit leaves the project file exactly as long, so the "
          "size half of the stamp cannot detect it");
    std::filesystem::last_write_time(cacheFile, stampedTime);

    Document cachedAfter;
    Check(cacheBackend.SnapshotDocument(cachedAfter, cacheMessage),
          "the second snapshot succeeds: " + cacheMessage);
    const DocumentClip* movedClip =
        cachedAfter.FindClip("01K30000000000000000000003");
    Check(movedClip != nullptr &&
              movedClip->timeline_in == RationalTime{9, 25},
          "a snapshot taken after an edit through the same backend sees the "
          "edit, with neither the file's size nor its timestamp to reveal it");

    server.Stop();
    std::filesystem::remove_all(directory);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All MCP integration tests passed\n";
    return 0;
}

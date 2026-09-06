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
#include "McpLiveBackend.h"
#include "McpServer.h"
#include "McpTools.h"

#include "Cli.h"
#include "Operations.h"
#include "Project.h"
#include "ShotQuality.h"
#include "SpeechOnset.h"
#include "Transcription.h"
#include "Ulid.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <optional>
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

void CheckFailureEnvelope(const McpToolCallOutcome& outcome,
                          const std::string& label,
                          const std::string& expectedError = std::string()) {
    mcp_json::Value envelope;
    std::string parseError;
    const bool parsed =
        mcp_json::Value::Parse(outcome.result_json, envelope, parseError) &&
        envelope.IsObject();
    Check(!outcome.ok, label + " is refused");
    Check(parsed, label + " returns a JSON object: " + parseError);
    if (!parsed) return;
    const mcp_json::Value* ok = envelope.Find("ok");
    const mcp_json::Value* error = envelope.Find("error");
    const mcp_json::Value* detail = envelope.Find("detail");
    Check(ok != nullptr && ok->IsBool() && !ok->AsBool(),
          label + " returns ok:false");
    Check(error != nullptr && error->IsString() && !error->AsString().empty() &&
              error->AsString() == outcome.error_name,
          label + " returns its stable error code");
    Check(detail != nullptr && detail->IsString() &&
              !detail->AsString().empty() &&
              detail->AsString() == outcome.message,
          label + " returns its detail");
    if (!expectedError.empty()) {
        Check(error != nullptr && error->IsString() &&
                  error->AsString() == expectedError,
              label + " preserves " + expectedError);
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
        {"01K30000000000000000000005",
         "audio",
         1,
         {{"01K30000000000000000000006",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}}}},
    };
    document.sequence.tracks[0].clips[0].link_group_id =
        "01K30000000000000000000005";
    document.sequence.tracks[0].clips[1].link_group_id =
        "01K30000000000000000000005";
    return document;
}

// In-memory McpBackend for testing: no project file, no ProjectStorage.
// ApplyOperation/Undo/Redo call EditLog::Apply/Undo/Redo directly against a
// Document held in memory -- the same function ApplyOperationCommand's
// `--apply-op` path calls.
class InMemoryBackend : public McpBackend {
public:
    explicit InMemoryBackend(Document document,
                             bool applyProjectOperations = false)
        : project_(Project::FromDocument(std::move(document))),
          apply_project_operations_(applyProjectOperations) {
        document_ = project_.MakeActiveDocument();
    }

    // Cache artifacts the real backends read from the project package. Held
    // here so the tools that consume them can be exercised without a
    // ProjectStorage dependency.
    void SetSourceTranscript(Transcript transcript) {
        transcript_ = std::move(transcript);
    }
    void SetSourceShotQuality(ShotQualityReport report) {
        shot_quality_ = std::move(report);
    }
    void SetSourceSpeechOnset(SpeechOnsetReport report) {
        speech_onset_ = std::move(report);
    }
    void SetTimelineTranscriptHallucinated(bool value) {
        timeline_transcript_hallucinated_ = value;
    }

    SpeechOnsetSettings speech_onset_settings;
    bool speech_onset_analysis_seen = false;

    bool AnalyzeSourceSpeechOnset(const Ulid&,
                                  const SpeechOnsetSettings& settings,
                                  std::string& resultJson,
                                  std::string&) override {
        speech_onset_settings = settings;
        speech_onset_analysis_seen = true;
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool ReadSourceSpeechOnset(const Ulid&, SpeechOnsetReport& report,
                               std::string& message) override {
        if (speech_onset_.levels.empty()) {
            message = "no speech envelope in this fixture";
            return false;
        }
        report = speech_onset_;
        return true;
    }

    bool ReadSourceTranscript(const Ulid&, Transcript& transcript,
                              std::string& message) override {
        if (transcript_.words.empty()) {
            message = "no transcript in this fixture";
            return false;
        }
        transcript = transcript_;
        return true;
    }

    // Records what the tool asked for, so the test can check the arguments
    // reach the engine rather than being swallowed by the dispatcher.
    struct TranscriptionRequest {
        std::vector<Ulid> media_ids;
        std::string language;
        bool verbatim = false;
        bool include_silent = false;
        bool seen = false;
    };
    TranscriptionRequest transcription_request;

    struct TimelineTranscriptionRequest {
        std::string timeline_id;
        std::string language;
        bool verbatim = false;
        bool seen = false;
    };
    TimelineTranscriptionRequest timeline_transcription_request;

    bool TranscribeSources(const std::vector<Ulid>& mediaIds,
                           const std::string& language, bool verbatim,
                           bool includeSilent, std::string& resultJson,
                           std::string& message) override {
        transcription_request = {mediaIds, language, verbatim, includeSilent,
                                 true};
        if (transcription_fails_) {
            message = "no Whisper model configured";
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool TranscribeTimeline(const std::string& timelineId,
                            const std::string& language, bool verbatim,
                            std::string& resultJson, std::string&) override {
        timeline_transcription_request = {timelineId, language, verbatim, true};
        resultJson = "{\"ok\":true}";
        return true;
    }

    void FailTranscription() { transcription_fails_ = true; }
    void RequireExplicitTimeline() { require_explicit_timeline_ = true; }

    bool SelectTimelineForEdit(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override {
        if (require_explicit_timeline_ && timelineId.empty()) {
            errorName = "TimelineRequired";
            message =
                "strict timeline editing requires an explicit "
                "timeline_id";
            return false;
        }
        return McpBackend::SelectTimelineForEdit(timelineId, errorName,
                                                 message);
    }

    // QC-2026-09 (A1) -- records whether the alignment pass was asked to
    // write, because that boolean is the whole difference between a report
    // and a transcript the word-level tools can be trusted on.
    struct AlignmentRequest {
        bool apply = false;
        bool seen = false;
    };
    AlignmentRequest alignment_request;

    bool AlignSourceTranscripts(bool apply, std::string& resultJson,
                                std::string&) override {
        alignment_request = {apply, true};
        resultJson = "{\"ok\":true,\"applied\":";
        resultJson += apply ? "true" : "false";
        resultJson += ",\"sources\":[]}";
        return true;
    }

    bool ReadSourceShotQuality(const Ulid&, ShotQualityReport& report,
                               std::string& message) override {
        if (shot_quality_.samples.empty()) {
            message = "no shot quality report in this fixture";
            return false;
        }
        report = shot_quality_;
        return true;
    }

    bool SnapshotDocument(Document& document, std::string&) override {
        document = document_;
        return true;
    }

    bool CaptureTimelineSheet(const TimelineSheetPlan& plan,
                              const TimelineSheetSettings&,
                              std::string& jpegBytes, std::string&) override {
        captured_sheet_ = plan;
        jpegBytes = "fake-jpeg";
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
        json = DescribeProject(project_);
        return true;
    }

    // S1 and S2 butt together; S3 sits after a gap. That shape is what lets
    // the tests below tell a legitimate merge from one that would silently
    // swallow a silence.
    bool ReadTimelineTranscript(std::string& json, std::string&) override {
        const std::string flag =
            timeline_transcript_hallucinated_ ? "true" : "false";
        json = R"({"timeline_id":"fixture","spans":[)";
        json +=
            R"({"span_id":"S1","source_id":"01K30000000000000000000001","source_in":{"value":100,"rate":25},"duration":{"value":10,"rate":25},"timeline_in":{"value":0,"rate":25},"text":"A strong hook.","likely_hallucinated":)" +
            flag + "},";
        json +=
            R"({"span_id":"S2","source_id":"01K30000000000000000000001","source_in":{"value":110,"rate":25},"duration":{"value":15,"rate":25},"timeline_in":{"value":10,"rate":25},"text":"It keeps going.","likely_hallucinated":)" +
            flag + "},";
        json +=
            R"({"span_id":"S3","source_id":"01K30000000000000000000001","source_in":{"value":200,"rate":25},"duration":{"value":12,"rate":25},"timeline_in":{"value":25,"rate":25},"text":"And the payoff.","likely_hallucinated":)" +
            flag + "}]}";
        return true;
    }

    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override {
        if (!apply_project_operations_) {
            project_operation_ = std::move(operation);
            resultJson = "{\"ok\":true}";
            return true;
        }
        EditError error = EditError::None;
        if (!project_log_.Apply(project_, std::move(operation), error,
                                message)) {
            errorName = EditErrorName(error);
            return false;
        }
        document_ = project_.MakeActiveDocument();
        project_operation_ = project_log_.AppliedEntries().back().op;
        resultJson = "{\"ok\":true}";
        return true;
    }

    const Document& CurrentDocument() const { return document_; }
    EditLog& Log() { return log_; }
    const std::optional<TimelineSheetPlan>& CapturedSheet() const {
        return captured_sheet_;
    }
    const ProjectOperation* LastProjectOperation() const {
        return project_operation_ ? &*project_operation_ : nullptr;
    }

private:
    bool transcription_fails_ = false;
    bool require_explicit_timeline_ = false;
    bool apply_project_operations_ = false;
    bool timeline_transcript_hallucinated_ = false;
    Document document_;
    EditLog log_;
    std::optional<ProjectOperation> project_operation_;
    Project project_;
    ProjectEditLog project_log_;
    Transcript transcript_;
    ShotQualityReport shot_quality_;
    SpeechOnsetReport speech_onset_;
    std::optional<TimelineSheetPlan> captured_sheet_;
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

    // B2 -- every registered tool crosses the same refusal boundary. A
    // non-object arguments value is invalid for all tools, including the
    // no-argument ones, so this walks the catalog without mutating the
    // fixture or depending on each tool's required fields.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const mcp_json::Value invalidArguments = mcp_json::Value::MakeArray();
        Check(!registry.Tools().empty(), "the MCP catalog is not empty");
        for (const McpTool& tool : registry.Tools()) {
            const McpToolCallOutcome refused =
                registry.Call(backend, tool.name, invalidArguments);
            CheckFailureEnvelope(refused, "catalog tool " + tool.name,
                                 "ValidationFailed");
        }
        const McpToolCallOutcome unknown =
            registry.Call(backend, "not_a_tool", invalidArguments);
        CheckFailureEnvelope(unknown, "unknown catalog tool", "UnknownTool");
    }

    IdResolver fixtureResolver(fixture);
    Ulid resolvedLinkGroup;
    std::string resolutionError;
    Check(fixtureResolver.Resolve("link_group_id", "01K30000000000000000000005",
                                  resolvedLinkGroup, resolutionError) &&
              resolvedLinkGroup == "01K30000000000000000000005",
          "ID resolver includes the shared IDs required by linked tools: " +
              resolutionError);
    Ulid resolvedAlias;
    Check(fixtureResolver.Resolve("clip_id", "A1", resolvedAlias,
                                  resolutionError) &&
              resolvedAlias == "01K30000000000000000000003",
          "ID resolver accepts the clip aliases exposed by describe: " +
              resolutionError);

    McpToolRegistry intentRegistry;
    InMemoryBackend intentBackend(fixture);
    mcp_json::Value previewArguments;
    std::string intentParseError;
    Check(
        mcp_json::Value::Parse(
            R"({"clip_id":"A1","edge":"Tail","amount":1,"unit":"Frames","preview":true})",
            previewArguments, intentParseError),
        "shorten intent preview arguments parse: " + intentParseError);
    const auto shortenTool = std::find_if(
        intentRegistry.Tools().begin(), intentRegistry.Tools().end(),
        [](const McpTool& tool) { return tool.name == "shorten_linked_clip"; });
    Check(shortenTool != intentRegistry.Tools().end() &&
              shortenTool->input_schema_json.find("timeline_id") !=
                  std::string::npos,
          "timeline editing schemas publish optional timeline_id");
    InMemoryBackend strictBackend(fixture);
    strictBackend.RequireExplicitTimeline();
    const McpToolCallOutcome strictRefusal = intentRegistry.Call(
        strictBackend, "shorten_linked_clip", previewArguments);
    CheckFailureEnvelope(strictRefusal, "strict edit without timeline",
                         "TimelineRequired");
    previewArguments.Set("timeline_id",
                         mcp_json::Value::MakeString(fixture.sequence.id));
    const McpToolCallOutcome strictExplicit = intentRegistry.Call(
        strictBackend, "shorten_linked_clip", previewArguments);
    Check(strictExplicit.ok, "strict edit accepts its explicit timeline: " +
                                 strictExplicit.message);
    previewArguments = mcp_json::Value::MakeObject();
    Check(
        mcp_json::Value::Parse(
            R"({"clip_id":"A1","edge":"Tail","amount":1,"unit":"Frames","preview":true})",
            previewArguments, intentParseError),
        "shorten preview arguments reset after strict check");
    const std::string intentBefore =
        intentBackend.CurrentDocument().SaveToString();
    const McpToolCallOutcome previewOutcome = intentRegistry.Call(
        intentBackend, "shorten_linked_clip", previewArguments);
    Check(previewOutcome.ok && previewOutcome.result_json.find(
                                   "\"preview\":true") != std::string::npos,
          "shorten_linked_clip previews one deterministic linked operation: " +
              previewOutcome.message);
    Check(intentBackend.CurrentDocument().SaveToString() == intentBefore,
          "shorten_linked_clip preview leaves the document byte-identical");

    previewArguments.Set("preview", mcp_json::Value::MakeBool(false));
    const McpToolCallOutcome shortenOutcome = intentRegistry.Call(
        intentBackend, "shorten_linked_clip", previewArguments);
    Check(shortenOutcome.ok,
          "shorten_linked_clip applies through the backend: " +
              shortenOutcome.message);
    Document intentExpected = fixture;
    EditLog intentExpectedLog;
    EditError intentExpectedError = EditError::None;
    std::string intentExpectedMessage;
    Check(intentExpectedLog.Apply(
              intentExpected,
              TrimLinkedClipsOperation{
                  "01K30000000000000000000005",
                  {{"01K30000000000000000000003", TrimEdge::Tail, {-1, 25}},
                   {"01K30000000000000000000004", TrimEdge::Tail, {-1, 25}}},
                  {}},
              intentExpectedError, intentExpectedMessage),
          "reference deterministic linked trim succeeds: " +
              intentExpectedMessage);
    Check(intentBackend.CurrentDocument().SaveToString() ==
              intentExpected.SaveToString(),
          "shorten_linked_clip computes IDs, sign and frame time exactly");

    InMemoryBackend backend(fixture);
    McpToolRegistry shortRegistry;

    // A6 -- both visual summaries return one MCP image with exact cell
    // metadata, without asking the caller to compute any timeline position.
    {
        const McpToolCallOutcome contact = shortRegistry.Call(
            backend, "contact_sheet", mcp_json::Value::MakeObject());
        Check(
            contact.ok && contact.image_mime == "image/jpeg" &&
                contact.image_base64 == "ZmFrZS1qcGVn" &&
                contact.result_json.find("\"role\":\"middle\"") !=
                    std::string::npos,
            "contact_sheet returns an image and exact middle-cell metadata: " +
                contact.message);

        Document adjacent = fixture;
        adjacent.sequence.tracks[0].clips[0].timeline_in = {0, 25};
        adjacent.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        InMemoryBackend cutBackend(adjacent);
        const McpToolCallOutcome cuts = shortRegistry.Call(
            cutBackend, "cut_sheet", mcp_json::Value::MakeObject());
        Check(cuts.ok && cuts.image_mime == "image/jpeg" &&
                  cuts.result_json.find("\"role\":\"before\"") !=
                      std::string::npos &&
                  cuts.result_json.find("\"role\":\"after\"") !=
                      std::string::npos &&
                  cuts.result_json.find(
                      "\"timeline_position\":{\"value\":9,\"rate\":25}") !=
                      std::string::npos,
              "cut_sheet returns exact adjacent frame pairs: " + cuts.message);

        mcp_json::Value invalid = mcp_json::Value::MakeObject();
        invalid.Set("max_images", mcp_json::Value::MakeInt(1));
        CheckFailureEnvelope(
            shortRegistry.Call(cutBackend, "cut_sheet", invalid),
            "cut sheet incomplete pair", "ValidationFailed");
    }

    // B11 -- image tools reject audio-only media by capability before a
    // backend starts FFmpeg and returns a generic I/O failure.
    {
        Document audioOnly;
        LibraryMedia media;
        media.id = "01K31000000000000000000001";
        media.path = "voice.wav";
        media.filename = "voice.wav";
        media.codec = "pcm_s16le";
        media.has_video = false;
        media.width = 0;
        media.height = 0;
        media.rate = {48000, 1};
        media.duration = {48000, 48000};
        media.orientation = "audio";
        media.has_audio = true;
        media.audio_rate = 48000;
        media.audio_channels = 1;
        audioOnly.library.push_back(media);
        audioOnly.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        InMemoryBackend audioBackend(audioOnly);
        McpToolRegistry audioRegistry;
        mcp_json::Value frameArguments;
        std::string frameParseError;
        Check(
            mcp_json::Value::Parse(
                R"({"media_id":"01K31000000000000000000001","source_in":{"value":0,"rate":48000}})",
                frameArguments, frameParseError),
            "audio-only frame arguments parse: " + frameParseError);
        const McpToolCallOutcome frameOutcome =
            audioRegistry.Call(audioBackend, "read_frame", frameArguments);
        CheckFailureEnvelope(frameOutcome, "read_frame on audio-only media",
                             "InvalidOperation");
        Check(frameOutcome.message.find("audio-only") != std::string::npos,
              "the frame refusal names the missing picture capability");
    }

    // B8 -- timelines are project objects, not an array an MCP client edits.
    // Exercise the tools through a real ProjectEditLog, including the engine's
    // stable refusal for the final timeline.
    {
        InMemoryBackend timelineBackend(fixture, true);
        McpToolRegistry timelineRegistry;
        const McpToolCallOutcome listed = timelineRegistry.Call(
            timelineBackend, "list_timelines", mcp_json::Value::MakeObject());
        Check(
            listed.ok &&
                listed.result_json.find("\"width\":1920") !=
                    std::string::npos &&
                listed.result_json.find("\"active\":true") != std::string::npos,
            "list_timelines exposes dimensions and active state");
        Document liveDocument = fixture;
        EditLog liveLog;
        Project liveProject = Project::FromDocument(fixture);
        bool liveDescriptionUsed = false;
        McpLiveBackend liveBackend(liveDocument, liveLog, nullptr, nullptr,
                                   [&](std::string& json, std::string&) {
                                       liveDescriptionUsed = true;
                                       json = DescribeProject(liveProject);
                                       return true;
                                   });
        const McpToolCallOutcome liveListed = timelineRegistry.Call(
            liveBackend, "list_timelines", mcp_json::Value::MakeObject());
        Check(liveListed.ok && liveDescriptionUsed,
              "the live chat backend lists timelines from its project view");
        mcp_json::Value addArguments;
        std::string timelineParseError;
        Check(
            mcp_json::Value::Parse(
                R"({"name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1}})",
                addArguments, timelineParseError),
            "add_timeline arguments parse: " + timelineParseError);
        Check(
            timelineRegistry.Call(timelineBackend, "add_timeline", addArguments)
                .ok,
            "add_timeline applies an AddProjectTimelineOperation");
        const ProjectOperation* added = timelineBackend.LastProjectOperation();
        Check(added &&
                  std::holds_alternative<AddProjectTimelineOperation>(*added),
              "add_timeline uses the existing project operation");
        const Ulid addedId =
            std::get<AddProjectTimelineOperation>(*added).timeline_id;
        mcp_json::Value renameArguments;
        Check(mcp_json::Value::Parse("{\"timeline_id\":\"" +
                                         addedId.substr(0, 10) +
                                         "\",\"name\":\"Portrait\"}",
                                     renameArguments, timelineParseError),
              "rename_timeline arguments parse: " + timelineParseError);
        Check(timelineRegistry
                  .Call(timelineBackend, "rename_timeline", renameArguments)
                  .ok,
              "rename_timeline resolves an unambiguous ULID prefix");
        Check(timelineBackend.LastProjectOperation() &&
                  std::holds_alternative<RenameProjectItemOperation>(
                      *timelineBackend.LastProjectOperation()),
              "rename_timeline uses RenameProjectItemOperation");
        mcp_json::Value removeArguments;
        Check(mcp_json::Value::Parse("{\"timeline_id\":\"" + addedId + "\"}",
                                     removeArguments, timelineParseError),
              "remove_timeline arguments parse: " + timelineParseError);
        Check(timelineRegistry
                  .Call(timelineBackend, "remove_timeline", removeArguments)
                  .ok,
              "remove_timeline applies a RemoveProjectTimelineOperation");
        Check(timelineBackend.LastProjectOperation() &&
                  std::holds_alternative<RemoveProjectTimelineOperation>(
                      *timelineBackend.LastProjectOperation()),
              "remove_timeline uses the existing project operation");
        const McpToolCallOutcome remaining = timelineRegistry.Call(
            timelineBackend, "list_timelines", mcp_json::Value::MakeObject());
        mcp_json::Value remainingJson;
        Check(mcp_json::Value::Parse(remaining.result_json, remainingJson,
                                     timelineParseError),
              "remaining timeline list parses: " + timelineParseError);
        const mcp_json::Value* remainingTimelines =
            remainingJson.Find("timelines");
        const std::string remainingId =
            remainingTimelines && !remainingTimelines->AsArray().empty()
                ? remainingTimelines->AsArray().front().Find("id")->AsString()
                : "";
        Check(
            mcp_json::Value::Parse("{\"timeline_id\":\"" + remainingId + "\"}",
                                   removeArguments, timelineParseError),
            "final removal arguments parse: " + timelineParseError);
        const McpToolCallOutcome finalRemoval = timelineRegistry.Call(
            timelineBackend, "remove_timeline", removeArguments);
        CheckFailureEnvelope(finalRemoval, "remove final timeline",
                             "InvalidOperation");
        Check(mcp_json::Value::Parse(
                  R"({"timeline_id":"01ZZZZZZZZZZZZZZZZZZZZZZZZ"})",
                  removeArguments, timelineParseError),
              "unknown timeline arguments parse: " + timelineParseError);
        const McpToolCallOutcome unknownTimeline = timelineRegistry.Call(
            timelineBackend, "remove_timeline", removeArguments);
        CheckFailureEnvelope(unknownTimeline, "remove unknown timeline",
                             "UnknownSequence");
    }
    const McpToolCallOutcome transcriptOutcome = shortRegistry.Call(
        backend, "get_timeline_transcript", mcp_json::Value::MakeObject());
    Check(transcriptOutcome.ok && transcriptOutcome.result_json.find(
                                      "A strong hook") != std::string::npos,
          "get_timeline_transcript returns the backend's semantic spans");

    // B1 -- the tool surfaces both measured groups and the derived onset,
    // and analysis settings reach the cache-producing backend unchanged.
    {
        Document speechDocument = fixture;
        DocumentTrack audio = speechDocument.sequence.tracks.front();
        audio.id = "01K30000000000000000000006";
        audio.kind = "audio";
        audio.clips.resize(1);
        audio.clips[0].id = "01K30000000000000000000007";
        audio.clips[0].source_in = {0, 50};
        audio.clips[0].duration = {160, 50};
        audio.clips[0].timeline_in = {0, 25};
        speechDocument.sequence.tracks.push_back(audio);

        SpeechOnsetReport report;
        report.media_id = "01K30000000000000000000001";
        report.windows_per_second = 50;
        report.decode_sample_rate = 16000;
        report.levels.insert(report.levels.end(), 50, 90000);
        report.levels.insert(report.levels.end(), 10, 1000);
        report.levels.insert(report.levels.end(), 100, 280000);
        report.speech_level = SpeechLevelPercentile(report.levels, 90);
        report.noise_floor = SpeechLevelPercentile(report.levels, 5);

        InMemoryBackend speechBackend(speechDocument);
        speechBackend.SetSourceSpeechOnset(report);
        McpToolRegistry speechRegistry;
        const McpToolCallOutcome speechList = speechRegistry.Call(
            speechBackend, "list_speech_onsets", mcp_json::Value::MakeObject());
        Check(speechList.ok &&
                  speechList.result_json.find("\"groups\":[{") !=
                      std::string::npos &&
                  speechList.result_json.find(
                      "\"dominant_onset\":{\"value\":60,\"rate\":50}") !=
                      std::string::npos,
              "list_speech_onsets publishes groups and dominant_onset");

        mcp_json::Value analyzeArguments;
        std::string analyzeError;
        Check(
            mcp_json::Value::Parse(
                R"({"media_id":"01K30000000000000000000001","group_gap_ms":300,"group_floor_db":8})",
                analyzeArguments, analyzeError),
            "speech analysis arguments parse: " + analyzeError);
        Check(
            speechRegistry
                    .Call(speechBackend, "analyze_speech_onset",
                          analyzeArguments)
                    .ok &&
                speechBackend.speech_onset_analysis_seen &&
                speechBackend.speech_onset_settings.thresholds
                        .group_gap_milliseconds == 300 &&
                speechBackend.speech_onset_settings.thresholds.group_floor_db ==
                    8,
            "analyze_speech_onset forwards configurable group settings");
    }
    // Segments are named by span id and resolved by the engine. A caller
    // that could state a source_in could state one landing mid-word, and
    // ApplyOperation would accept it: it only checks that a range sits
    // inside the media. Refusing the field is what makes that unbuildable.
    const auto callShort = [&](const std::string& argumentsJson) {
        mcp_json::Value arguments;
        std::string parseFailure;
        Check(mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
              "short tool arguments parse: " + parseFailure);
        return shortRegistry.Call(backend, "create_interview_short", arguments);
    };
    const auto capturedSegments = [&]() {
        const ProjectOperation* captured = backend.LastProjectOperation();
        return captured && std::holds_alternative<
                               CreateProjectTimelineFromSegmentsOperation>(
                               *captured)
                   ? std::get<CreateProjectTimelineFromSegmentsOperation>(
                         *captured)
                         .segments
                   : std::vector<ProjectTimelineSourceSegment>{};
    };

    Check(
        callShort(R"({"name":"Short test","segments":[{"span_id":"S1"}]})").ok,
        "create_interview_short accepts a span id");
    Check(capturedSegments().size() == 1 &&
              capturedSegments()[0].source_in == RationalTime{100, 25} &&
              capturedSegments()[0].duration == RationalTime{10, 25} &&
              capturedSegments()[0].source_id == "01K30000000000000000000001",
          "a span id resolves to that span's exact source range");

    Check(callShort(R"({"segments":[{"span_id":"S1","end_span_id":"S2"}]})").ok,
          "create_interview_short accepts a contiguous span run");
    Check(capturedSegments().size() == 1 &&
              capturedSegments()[0].source_in == RationalTime{100, 25} &&
              capturedSegments()[0].duration == RationalTime{25, 25},
          "a contiguous run merges into one exact range, computed by the "
          "engine rather than added up by the caller");

    const McpToolCallOutcome gapOutcome =
        callShort(R"({"segments":[{"span_id":"S1","end_span_id":"S3"}]})");
    Check(!gapOutcome.ok &&
              gapOutcome.message.find("breath") != std::string::npos,
          "a run spanning a real silence is refused, not stitched over");
    CheckFailureEnvelope(gapOutcome, "non-contiguous interview span",
                         "ValidationFailed");

    Check(!callShort(R"({"segments":[{"span_id":"S9"}]})").ok,
          "an unknown span id is refused");

    backend.SetTimelineTranscriptHallucinated(true);
    const McpToolCallOutcome unsafeShort =
        callShort(R"({"segments":[{"span_id":"S1"}]})");
    CheckFailureEnvelope(unsafeShort, "hallucinated interview transcript",
                         "ValidationFailed");
    Check(unsafeShort.message.find("force:true") != std::string::npos,
          "the interview refusal names the explicit override");
    Check(callShort(R"({"segments":[{"span_id":"S1"}],"force":true})").ok,
          "create_interview_short accepts an explicit hallucination "
          "override");
    backend.SetTimelineTranscriptHallucinated(false);

    // The regression this whole shape exists to prevent.
    Check(
        !callShort(
             R"({"segments":[{"source_id":"01K30000000000000000000001","source_in":{"value":103,"rate":25},"duration":{"value":10,"rate":25}}]})")
             .ok,
        "a raw source range is refused: the model never states a time");

    // B12 -- a transcript whose words overlap no measured speech group is
    // visible for diagnosis but cannot drive word cuts without an explicit
    // override. Both the read-only proposal and the mutating tool enforce the
    // same cache marker.
    {
        Transcript unsafeTranscript;
        unsafeTranscript.media_id = "01K30000000000000000000001";
        unsafeTranscript.whisper_model = "large-v3";
        unsafeTranscript.source_rate = {25, 1};
        unsafeTranscript.speech_assessed = true;
        unsafeTranscript.measured_speech_duration = {0, 50};
        unsafeTranscript.likely_hallucinated = true;
        unsafeTranscript.known_hallucination_phrase = true;
        unsafeTranscript.words = {
            {"Sous-titrage", {100, 25}, {102, 25}},
            {"Société", {102, 25}, {104, 25}},
        };
        InMemoryBackend guardedBackend(fixture);
        guardedBackend.SetSourceTranscript(unsafeTranscript);
        McpToolRegistry guardedRegistry;
        mcp_json::Value listArguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000003"})", listArguments,
                  parseError),
              "guarded list arguments parse: " + parseError);
        const McpToolCallOutcome refusedList = guardedRegistry.Call(
            guardedBackend, "list_disfluencies", listArguments);
        CheckFailureEnvelope(refusedList, "hallucinated disfluency list",
                             "ValidationFailed");
        Check(refusedList.message.find("force:true") != std::string::npos,
              "the disfluency refusal names the explicit override");
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000003","force":true})",
                  listArguments, parseError) &&
                  guardedRegistry
                      .Call(guardedBackend, "list_disfluencies", listArguments)
                      .ok,
              "list_disfluencies accepts force:true");

        mcp_json::Value removeArguments;
        Check(
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000003","ranges":[{"start_word_index":0,"end_word_index":0}]})",
                removeArguments, parseError),
            "guarded removal arguments parse: " + parseError);
        const McpToolCallOutcome refusedRemoval = guardedRegistry.Call(
            guardedBackend, "remove_words", removeArguments);
        CheckFailureEnvelope(refusedRemoval, "hallucinated word removal",
                             "ValidationFailed");
        Check(
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000003","ranges":[{"start_word_index":0,"end_word_index":0}],"force":true})",
                removeArguments, parseError) &&
                guardedRegistry
                    .Call(guardedBackend, "remove_words", removeArguments)
                    .ok,
            "remove_words accepts force:true");
    }

    // ---- clean_disfluencies: one intention, one reversible operation ----
    // The clip covers source frames [100, 110) at 25/s, so every word below
    // sits inside it and the "euh" is the only filler.
    Transcript transcript;
    transcript.media_id = "01K30000000000000000000001";
    transcript.whisper_model = "large-v3";
    transcript.verbatim = true;
    transcript.source_rate = {25, 1};
    transcript.words = {
        {"Le", {100, 25}, {102, 25}},
        {"euh", {102, 25}, {104, 25}},
        {"montage", {104, 25}, {107, 25}},
        {"avance", {107, 25}, {110, 25}},
    };

    InMemoryBackend cleanBackend(fixture);
    cleanBackend.SetSourceTranscript(transcript);
    McpToolRegistry cleanRegistry;
    const std::string beforeClean =
        cleanBackend.CurrentDocument().SaveToString();
    mcp_json::Value cleanArguments;
    std::string cleanParseError;
    Check(mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000003"})",
                                 cleanArguments, cleanParseError),
          "clean_disfluencies arguments parse: " + cleanParseError);
    const McpToolCallOutcome cleanOutcome =
        cleanRegistry.Call(cleanBackend, "clean_disfluencies", cleanArguments);
    Check(cleanOutcome.ok,
          "clean_disfluencies applies: " + cleanOutcome.message);
    Check(cleanOutcome.result_json.find("\"removed_count\":1") !=
                  std::string::npos &&
              cleanOutcome.result_json.find("euh") != std::string::npos,
          "clean_disfluencies reports the filler it cut, by text");
    Check(
        cleanOutcome.result_json.find("\"verbatim\":true") != std::string::npos,
        "clean_disfluencies reports which decoding the transcript came "
        "from, because a standard one would say nothing about the take");
    Check(cleanBackend.CurrentDocument().SaveToString() != beforeClean,
          "clean_disfluencies actually changed the document");

    // One tool call is one operation, so one undo restores the document
    // byte for byte -- the property that makes this safe to point at an
    // agent in the first place.
    std::string cleanUndoErrorName;
    std::string cleanUndoMessage;
    std::string cleanUndoResult;
    Check(cleanBackend.Undo(cleanUndoResult, cleanUndoErrorName,
                            cleanUndoMessage),
          "the cleanup undoes: " + cleanUndoMessage);
    Check(cleanBackend.CurrentDocument().SaveToString() == beforeClean,
          "undoing one clean_disfluencies call restores the exact prior "
          "bytes, so the whole cleanup is a single reversible step");

    // Cleaning a clip with nothing to cut is an honoured request, not an
    // error, and must not touch the document.
    Transcript clean;
    clean.media_id = "01K30000000000000000000001";
    clean.whisper_model = "large-v3";
    clean.verbatim = true;
    clean.source_rate = {25, 1};
    clean.words = {{"Le", {100, 25}, {104, 25}},
                   {"montage", {104, 25}, {110, 25}}};
    InMemoryBackend tidyBackend(fixture);
    tidyBackend.SetSourceTranscript(clean);
    const std::string beforeTidy = tidyBackend.CurrentDocument().SaveToString();
    const McpToolCallOutcome tidyOutcome =
        cleanRegistry.Call(tidyBackend, "clean_disfluencies", cleanArguments);
    Check(tidyOutcome.ok && tidyOutcome.result_json.find("\"applied\":false") !=
                                std::string::npos,
          "cleaning an already clean clip succeeds without applying anything");
    Check(tidyBackend.CurrentDocument().SaveToString() == beforeTidy,
          "a no-op cleanup leaves the document untouched");

    // An A/V pair is resolved by the tool, not by the caller: naming the
    // audio clip must take its linked picture with it, in one operation.
    Document avFixture;
    avFixture.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}}};
    const auto linkedTrack = [](const Ulid& trackId, const std::string& kind,
                                int32_t index, const Ulid& clipId) {
        DocumentTrack track;
        track.id = trackId;
        track.kind = kind;
        track.index = index;
        DocumentClip clip{
            clipId, "01K30000000000000000000001", {100, 25}, {10, 25}, {0, 25}};
        clip.link_group_id = "01K30000000000000000000050";
        track.clips = {clip};
        return track;
    };
    avFixture.sequence.tracks = {
        linkedTrack("01K30000000000000000000051", "video", 0,
                    "01K30000000000000000000052"),
        linkedTrack("01K30000000000000000000053", "audio", 1,
                    "01K30000000000000000000054"),
    };

    InMemoryBackend avBackend(avFixture);
    avBackend.SetSourceTranscript(transcript);
    mcp_json::Value avArguments;
    std::string avParseError;
    Check(mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000054"})",
                                 avArguments, avParseError),
          "A/V cleanup arguments parse: " + avParseError);
    const std::string beforeAv = avBackend.CurrentDocument().SaveToString();
    const McpToolCallOutcome avOutcome =
        cleanRegistry.Call(avBackend, "clean_disfluencies", avArguments);
    Check(avOutcome.ok,
          "clean_disfluencies applies to a pair: " + avOutcome.message);
    Check(avOutcome.result_json.find("01K30000000000000000000052") !=
              std::string::npos,
          "the result names the linked picture it also cut, so the caller "
          "can see it happened");
    const DocumentTrack& avVideo =
        avBackend.CurrentDocument().sequence.tracks[0];
    const DocumentTrack& avAudio =
        avBackend.CurrentDocument().sequence.tracks[1];
    Check(avVideo.clips.size() == avAudio.clips.size(),
          "picture and sound end up with the same number of fragments");
    bool alignedPair = avVideo.clips.size() == avAudio.clips.size();
    for (size_t index = 0; index < avVideo.clips.size() && alignedPair; ++index)
        alignedPair =
            avVideo.clips[index].timeline_in ==
                avAudio.clips[index].timeline_in &&
            avVideo.clips[index].duration == avAudio.clips[index].duration;
    Check(alignedPair,
          "and they stay frame-aligned: cleaning the sound did not slide it "
          "out from under the picture");

    std::string avUndoResult;
    std::string avUndoErrorName;
    std::string avUndoMessage;
    Check(avBackend.Undo(avUndoResult, avUndoErrorName, avUndoMessage) &&
              avBackend.CurrentDocument().SaveToString() == beforeAv,
          "one undo restores both tracks: the pair cut is a single step");

    // ---- list_shot_quality: measured, never judged --------------------
    // Forty samples at 4/s. The take is sharp and still except at samples
    // 32 and 33, which is where the timeline's second clip sits.
    ShotQualityReport quality;
    quality.media_id = "01K30000000000000000000001";
    quality.samples_per_second = 4;
    quality.analysis_width = 384;
    quality.analysis_height = 216;
    std::vector<int64_t> allSharpness;
    for (int index = 0; index < 40; ++index) {
        const bool ruined = index == 32 || index == 33;
        ShotQualitySample sample;
        sample.time = RationalTime{index, 4};
        sample.sharpness = ruined ? 900 : 4000;
        sample.motion = ruined ? 130000 : 2000;
        allSharpness.push_back(sample.sharpness);
        quality.samples.push_back(sample);
    }
    quality.median_sharpness = ShotQualityPercentile(allSharpness, 50);

    InMemoryBackend qualityBackend(fixture);
    qualityBackend.SetSourceShotQuality(quality);
    McpToolRegistry qualityRegistry;
    const McpToolCallOutcome qualityOutcome = qualityRegistry.Call(
        qualityBackend, "list_shot_quality", mcp_json::Value::MakeObject());
    Check(qualityOutcome.ok,
          "list_shot_quality reports: " + qualityOutcome.message);
    mcp_json::Value qualityView;
    std::string qualityParseError;
    Check(mcp_json::Value::Parse(qualityOutcome.result_json, qualityView,
                                 qualityParseError),
          "shot quality view parses: " + qualityParseError);
    const mcp_json::Value* qualityClips = qualityView.Find("clips");
    Check(qualityClips && qualityClips->IsArray() &&
              qualityClips->AsArray().size() == 2,
          "both video clips are graded");
    bool sawClean = false;
    bool sawRuined = false;
    if (qualityClips && qualityClips->IsArray()) {
        for (const mcp_json::Value& entry : qualityClips->AsArray()) {
            const mcp_json::Value* sharpness = entry.Find("sharpness");
            const mcp_json::Value* steadiness = entry.Find("steadiness");
            const mcp_json::Value* isClean = entry.Find("clean");
            if (!sharpness || !steadiness || !isClean) continue;
            if (isClean->AsBool() && sharpness->AsString() == "Sharp" &&
                steadiness->AsString() == "Steady")
                sawClean = true;
            if (!isClean->AsBool() && sharpness->AsString() == "Blurry" &&
                steadiness->AsString() == "Shaky")
                sawRuined = true;
        }
    }
    Check(sawClean, "the usable clip grades Sharp and Steady");
    Check(sawRuined,
          "the clip sitting over the ruined samples grades Blurry and Shaky");

    // Occlusion: a clip covered end to end by a higher video track is not a
    // defect to fix, because nobody sees it. The grade must still say it
    // measured badly -- what changes is whether it needs acting on.
    Document coveredFixture = fixture;
    DocumentTrack cover;
    cover.id = "01K30000000000000000000060";
    cover.kind = "video";
    cover.index = 5;  // above track 0, so composited over it
    // Covers the second clip's whole span (timeline 20..30 at 25/s) and only
    // the first half of the first clip's (5..10 of 5..15).
    cover.clips = {{"01K30000000000000000000061",
                    "01K30000000000000000000001",
                    {0, 25},
                    {10, 25},
                    {20, 25}},
                   {"01K30000000000000000000062",
                    "01K30000000000000000000001",
                    {0, 25},
                    {5, 25},
                    {5, 25}}};
    coveredFixture.sequence.tracks.push_back(cover);

    InMemoryBackend coveredBackend(coveredFixture);
    // Soft only where the two clips actually read -- source seconds 4.0 and
    // 8.0, i.e. samples 16-17 and 32-33 on the 4/s grid. Making the whole
    // source soft instead would move its own median down onto the soft value
    // and grade everything Sharp, which is the documented cost of a
    // source-relative grade, not the thing under test here.
    ShotQualityReport poor;
    poor.media_id = "01K30000000000000000000001";
    poor.samples_per_second = 4;
    poor.analysis_width = 384;
    poor.analysis_height = 216;
    std::vector<int64_t> poorSharpness;
    for (int index = 0; index < 40; ++index) {
        ShotQualitySample sample;
        sample.time = RationalTime{index, 4};
        const bool insideAClip =
            index == 16 || index == 17 || index == 32 || index == 33;
        sample.sharpness = insideAClip ? 800 : 4000;
        sample.motion = 2000;
        poorSharpness.push_back(sample.sharpness);
        poor.samples.push_back(sample);
    }
    poor.median_sharpness = ShotQualityPercentile(poorSharpness, 50);
    poor.median_motion = 2000;
    coveredBackend.SetSourceShotQuality(poor);

    const McpToolCallOutcome coveredOutcome = qualityRegistry.Call(
        coveredBackend, "list_shot_quality", mcp_json::Value::MakeObject());
    mcp_json::Value coveredView;
    std::string coveredParseError;
    Check(coveredOutcome.ok &&
              mcp_json::Value::Parse(coveredOutcome.result_json, coveredView,
                                     coveredParseError),
          "the covered timeline reports: " + coveredParseError);
    int fullyCovered = 0;
    int needsAttention = 0;
    int gradedBad = 0;
    const mcp_json::Value* coveredClips = coveredView.Find("clips");
    if (coveredClips && coveredClips->IsArray()) {
        for (const mcp_json::Value& entry : coveredClips->AsArray()) {
            const mcp_json::Value* isClean = entry.Find("clean");
            const mcp_json::Value* covered = entry.Find("fully_covered");
            const mcp_json::Value* attention = entry.Find("needs_attention");
            if (!isClean || !covered || !attention) continue;
            if (!isClean->AsBool()) ++gradedBad;
            if (covered->AsBool()) ++fullyCovered;
            if (attention->AsBool()) ++needsAttention;
        }
    }
    Check(gradedBad >= 2,
          "both clips of the covered fixture still measure badly");
    Check(fullyCovered == 1,
          "exactly the clip covered end to end reports fully_covered");
    Check(needsAttention == gradedBad - 1,
          "the covered one drops out of needs_attention while the partly "
          "visible one stays: occlusion changes what to act on, never the "
          "grade");

    // A source that was never analysed must read as unknown, never as a
    // pass: a caller filtering on `clean` would otherwise cut with it.
    InMemoryBackend blindBackend(fixture);
    const McpToolCallOutcome blindOutcome = qualityRegistry.Call(
        blindBackend, "list_shot_quality", mcp_json::Value::MakeObject());
    mcp_json::Value blindView;
    std::string blindParseError;
    Check(blindOutcome.ok && mcp_json::Value::Parse(blindOutcome.result_json,
                                                    blindView, blindParseError),
          "an unanalysed timeline still reports: " + blindParseError);
    const mcp_json::Value* blindClips = blindView.Find("clips");
    const mcp_json::Value* blindUnknown = blindView.Find("unanalyzed");
    Check(blindClips && blindClips->IsArray() && blindClips->AsArray().empty(),
          "nothing is graded without an analysis");
    Check(blindUnknown && blindUnknown->IsArray() &&
              blindUnknown->AsArray().size() == 2,
          "every unmeasured clip is named as unmeasured rather than omitted");

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
    bool sawSetColorManagement = false;
    bool sawSetClipOpacity = false;
    bool sawSetClipAudio = false;
    bool sawContactSheet = false;
    bool sawCutSheet = false;
    bool sawUndo = false;
    if (toolsField) {
        for (const mcp_json::Value& tool : toolsField->AsArray()) {
            const mcp_json::Value* name = tool.Find("name");
            if (!name) continue;
            if (name->AsString() == "insert_clip") sawInsertClip = true;
            if (name->AsString() == "clear_clips") sawClearClips = true;
            if (name->AsString() == "trim_clip") sawTrimClip = true;
            if (name->AsString() == "set_color_management")
                sawSetColorManagement = true;
            if (name->AsString() == "set_clip_opacity")
                sawSetClipOpacity = true;
            if (name->AsString() == "set_clip_audio") sawSetClipAudio = true;
            if (name->AsString() == "contact_sheet") sawContactSheet = true;
            if (name->AsString() == "cut_sheet") sawCutSheet = true;
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
    Check(sawSetColorManagement, "tools/list includes set_color_management");
    Check(sawSetClipOpacity, "tools/list includes set_clip_opacity");
    Check(sawSetClipAudio, "tools/list includes set_clip_audio");
    Check(sawContactSheet, "tools/list includes contact_sheet");
    Check(sawCutSheet, "tools/list includes cut_sheet");
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

    const std::string opacityRequest =
        std::string(R"({"jsonrpc":"2.0","id":11,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_opacity","arguments":{"clip_id":")" +
        insertedClipId + R"(","opacity":{"num":3,"den":5}}}})";
    const std::string opacityResponse =
        HttpPostJson(server.Port(), "/mcp", opacityRequest);
    Check(opacityResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_opacity is not an error");
    Check(expectedLog.Apply(expected,
                            SetClipOpacityOperation{insertedClipId, {3, 5}},
                            expectedError, expectedMessage),
          "reference direct EditLog::Apply(opacity) succeeds: " +
              expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_clip_opacity matches a direct operation");

    const std::string audioRequest =
        std::string(R"({"jsonrpc":"2.0","id":12,"method":"tools/call",)") +
        R"("params":{"name":"set_clip_audio","arguments":{"clip_id":")" +
        R"(01K30000000000000000000006","gain_db":{"num":10,"den":1},)"
        R"("fade_in":{"value":2,"rate":25},"fade_out":{"value":3,"rate":25}}}})";
    const std::string audioResponse =
        HttpPostJson(server.Port(), "/mcp", audioRequest);
    Check(audioResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_clip_audio is not an error");
    Check(
        expectedLog.Apply(
            expected,
            SetClipAudioOperation{
                "01K30000000000000000000006", {10, 1}, {2, 25}, {3, 25}},
            expectedError, expectedMessage),
        "reference direct EditLog::Apply(audio) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_clip_audio matches a direct operation");

    // ---- tools/call: project color state through the shared operation ----
    const std::string colorRequest =
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call",)"
        R"("params":{"name":"set_color_management","arguments":{)"
        R"("enabled":true,"input_gamut":"sony_sgamut3_cine",)"
        R"("input_transfer":"sony_slog3","input_ycbcr_matrix":"bt709",)"
        R"("input_range":"full","working_gamut":"acescct",)"
        R"("output_gamut":"rec2020","output_transfer":"hlg"}}})";
    const std::string colorResponse =
        HttpPostJson(server.Port(), "/mcp", colorRequest);
    Check(colorResponse.find("\"isError\":false") != std::string::npos,
          "tools/call set_color_management is not an error");
    ColorManagementSettings colorSettings;
    colorSettings.enabled = true;
    colorSettings.input_gamut = "sony_sgamut3_cine";
    colorSettings.input_transfer = "sony_slog3";
    colorSettings.input_ycbcr_matrix = "bt709";
    colorSettings.input_range = "full";
    colorSettings.working_gamut = "acescct";
    colorSettings.output_gamut = "rec2020";
    colorSettings.output_transfer = "hlg";
    Check(
        expectedLog.Apply(expected, SetColorManagementOperation{colorSettings},
                          expectedError, expectedMessage),
        "reference direct EditLog::Apply(color) succeeds: " + expectedMessage);
    Check(backend.CurrentDocument().SaveToString() == expected.SaveToString(),
          "set_color_management matches a direct operation");

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
    bool badTextIsEnvelope = false;
    if (badResult) {
        const mcp_json::Value* isError = badResult->Find("isError");
        badIsError = isError && isError->IsBool() && isError->AsBool();
        const mcp_json::Value* content = badResult->Find("content");
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty()) {
            const mcp_json::Value* text = content->AsArray()[0].Find("text");
            mcp_json::Value envelope;
            std::string envelopeError;
            if (text != nullptr && text->IsString() &&
                mcp_json::Value::Parse(text->AsString(), envelope,
                                       envelopeError) &&
                envelope.IsObject()) {
                const mcp_json::Value* ok = envelope.Find("ok");
                const mcp_json::Value* error = envelope.Find("error");
                const mcp_json::Value* detail = envelope.Find("detail");
                badTextIsEnvelope = ok != nullptr && ok->IsBool() &&
                                    !ok->AsBool() && error != nullptr &&
                                    error->IsString() &&
                                    error->AsString() == "ValidationFailed" &&
                                    detail != nullptr && detail->IsString() &&
                                    !detail->AsString().empty();
            }
        }
    }
    Check(badIsError,
          "an unknown clip_id is reported as isError, not a "
          "JSON-RPC protocol error");
    Check(badTextIsEnvelope,
          "a refused tools/call exposes an ok/error/detail JSON object");
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

    // SEQ-2026-08 -- conform_sequence. The agent names the intent; the
    // engine computes the pixels, rotation included.
    {
        Document rotated = fixture;
        rotated.sequence.width = 1920;
        rotated.sequence.height = 1080;
        rotated.sequence.frame_rate = {25, 1};
        rotated.library.clear();
        // Document::Validate requires a path and a filename on every library
        // entry, so the fixture carries them: the operation is applied for
        // real, not against a document the engine would reject anyway.
        const auto rush = [](int32_t rateNum, const std::string& name) {
            LibraryMedia media;
            media.id = GenerateUlid();
            media.path = "rushes/" + name;
            media.filename = name;
            media.codec = "h264";
            media.width = 3840;
            media.height = 2160;
            media.rotation_degrees = 90;
            media.orientation = "portrait";
            media.rate = {rateNum, 1};
            media.duration = {100, rateNum};
            return media;
        };
        rotated.library.push_back(rush(25, "A.MP4"));
        rotated.library.push_back(rush(25, "B.MP4"));
        rotated.library.push_back(rush(25, "C.MP4"));
        rotated.library.push_back(rush(50, "D.MP4"));

        McpToolRegistry registry;
        InMemoryBackend backend(rotated);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "conform_sequence arguments parse: " + parseFailure);
            return registry.Call(backend, "conform_sequence", arguments);
        };

        const McpToolCallOutcome preview = call(R"({"preview":true})");
        Check(preview.ok, "conform_sequence previews: " + preview.message);
        Check(preview.result_json.find("\"width\":2160") != std::string::npos &&
                  preview.result_json.find("\"height\":3840") !=
                      std::string::npos,
              "the proposal is the displayed frame, not the stored one: " +
                  preview.result_json);
        Check(
            preview.result_json.find("\"applied\":false") != std::string::npos,
            "a preview reports that it changed nothing");
        Document afterPreview;
        std::string snapshotMessage;
        backend.SnapshotDocument(afterPreview, snapshotMessage);
        Check(afterPreview.sequence.width == 1920,
              "a preview really does leave the sequence alone");
        Check(preview.result_json.find("\"media_count\":3") !=
                      std::string::npos &&
                  preview.result_json.find("\"media_count\":1") !=
                      std::string::npos,
              "both formats are reported with their counts: " +
                  preview.result_json);

        const McpToolCallOutcome applied = call(R"({})");
        Check(applied.ok, "conform_sequence applies: " + applied.error_name +
                              " " + applied.message);
        Check(applied.result_json.find("\"applied\":true") != std::string::npos,
              "applying says so: " + applied.result_json);
        Document conformed;
        backend.SnapshotDocument(conformed, snapshotMessage);
        Check(conformed.sequence.width == 2160 &&
                  conformed.sequence.height == 3840 &&
                  conformed.sequence.frame_rate.num == 25 &&
                  conformed.sequence.frame_rate.den == 1,
              "the sequence now matches the rushes exactly");
        Check(conformed.sequence.name == fixture.sequence.name,
              "conforming the format does not rename the sequence");

        // A second call must not push a no-op onto the undo stack.
        const McpToolCallOutcome again = call(R"({})");
        Check(again.ok &&
                  again.result_json.find("\"already_conformed\":true") !=
                      std::string::npos &&
                  again.result_json.find("\"applied\":false") !=
                      std::string::npos,
              "a sequence already matching is reported, not rewritten: " +
                  again.result_json);
        Check(registry.Call(backend, "undo", mcp_json::Value::MakeObject()).ok,
              "one undo is enough to take the format back");
        Document undone;
        backend.SnapshotDocument(undone, snapshotMessage);
        Check(undone.sequence.width == 1920 && undone.sequence.height == 1080,
              "undo restores the previous format exactly");

        Check(!call(R"({"previw":true})").ok,
              "a misspelled argument is refused rather than ignored");

        // A project with nothing to derive a format from must say so.
        Document soundOnly = fixture;
        soundOnly.library.clear();
        InMemoryBackend silent(soundOnly);
        const McpToolCallOutcome nothing = registry.Call(
            silent, "conform_sequence", mcp_json::Value::MakeObject());
        Check(!nothing.ok &&
                  nothing.message.find("picture format") != std::string::npos,
              "an empty library refuses with a reason: " + nothing.message);
    }

    // ALPHA-2026-08 -- transcribe_media. The tool the catalog lacked while
    // every word-level tool in it needed a transcript to exist.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "transcribe_media arguments parse: " + parseFailure);
            return registry.Call(backend, "transcribe_media", arguments);
        };

        const McpToolCallOutcome outcome = call(
            R"({"media_id":"01K30000000000000000000001","language":"fr","verbatim":true})");
        Check(outcome.ok, "transcribe_media succeeds: " + outcome.message);
        Check(backend.transcription_request.seen &&
                  backend.transcription_request.media_ids.size() == 1 &&
                  backend.transcription_request.media_ids[0] ==
                      "01K30000000000000000000001" &&
                  backend.transcription_request.language == "fr" &&
                  backend.transcription_request.verbatim,
              "media, language and verbatim reach the engine unchanged");

        // QC-2026-09 A3 -- the batch form is the whole point of the ticket:
        // one model load for the rushes instead of one per rush.
        backend.transcription_request = {};
        Check(call(R"({"media_ids":["01K30000000000000000000001"]})").ok,
              "a list of media is accepted");
        Check(backend.transcription_request.media_ids.size() == 1,
              "and reaches the engine as a batch");
        Check(!call(R"({"media_id":"01K30000000000000000000001",)"
                    R"("media_ids":["01K30000000000000000000001"]})")
                   .ok,
              "naming both forms is refused rather than resolved arbitrarily");
        backend.transcription_request = {};
        Check(call(R"({"media_id":"01K30000000000000000000001",)"
                   R"("include_silent":true})")
                      .ok &&
                  backend.transcription_request.include_silent,
              "the override for media measured as mute reaches the engine");

        backend.transcription_request = {};
        Check(call(R"({"media_id":"01K30000000000000000000001"})").ok,
              "language and verbatim are optional");
        Check(backend.transcription_request.language == "auto" &&
                  !backend.transcription_request.verbatim,
              "the defaults are automatic detection and non-verbatim");

        Check(
            !call(
                 R"({"media_id":"01K30000000000000000000001","langauge":"fr"})")
                 .ok,
            "a misspelled argument is refused rather than ignored");
        Check(!call(R"({})").ok, "a media to transcribe is required");
        Check(!call(R"({"media_id":"01K39999999999999999999999"})").ok,
              "an unknown media is refused");

        // The message a user must act on: the model is a local setting, and
        // a failure that does not say so leaves them with nothing to do.
        InMemoryBackend unconfigured(fixture);
        unconfigured.FailTranscription();
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"media_id":"01K30000000000000000000001"})",
                               arguments, parseFailure);
        const McpToolCallOutcome failure =
            registry.Call(unconfigured, "transcribe_media", arguments);
        Check(!failure.ok &&
                  failure.message.find("Whisper model") != std::string::npos,
              "an unconfigured model surfaces as a named reason");
    }

    // B7 -- timeline transcription has its own address and preserves the
    // language/model-mode cache identity inputs through MCP.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        mcp_json::Value arguments;
        std::string parseError;
        Check(
            mcp_json::Value::Parse(
                R"({"timeline_id":"01K30000000000000000000020","language":"fr","verbatim":true})",
                arguments, parseError),
            "transcribe_timeline arguments parse: " + parseError);
        const McpToolCallOutcome outcome =
            registry.Call(backend, "transcribe_timeline", arguments);
        Check(outcome.ok && backend.timeline_transcription_request.seen &&
                  backend.timeline_transcription_request.timeline_id ==
                      "01K30000000000000000000020" &&
                  backend.timeline_transcription_request.language == "fr" &&
                  backend.timeline_transcription_request.verbatim,
              "timeline id, language and verbatim reach timeline "
              "transcription unchanged");

        arguments = mcp_json::Value::MakeObject();
        Check(registry.Call(backend, "transcribe_timeline", arguments).ok &&
                  backend.timeline_transcription_request.timeline_id.empty() &&
                  backend.timeline_transcription_request.language == "auto" &&
                  !backend.timeline_transcription_request.verbatim,
              "timeline transcription defaults to the active timeline and "
              "automatic language detection");
    }

    // QC-2026-09 A4 -- addressing by source frame. The fixture's clip A1
    // plays source frames 100..109 at timeline 5..14, so a caller naming a
    // rush frame must land on the position the engine computes and never on
    // the frame number itself.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& tool,
                              const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                tool + " arguments parse: " + parseFailure);
            return registry.Call(backend, tool, arguments);
        };

        const McpToolCallOutcome located = call(
            "locate_source_frame",
            R"({"media_id":"01K30000000000000000000001","source_frame":105})");
        Check(located.ok, "locate_source_frame succeeds: " + located.message);
        mcp_json::Value view;
        std::string viewError;
        Check(mcp_json::Value::Parse(located.result_json, view, viewError),
              "its result is JSON: " + viewError);
        const mcp_json::Value* matches = view.Find("matches");
        Check(matches != nullptr && matches->IsArray() &&
                  matches->AsArray().size() == 2,
              "the linked video and audio clips play that frame");
        if (matches != nullptr && matches->AsArray().size() == 2) {
            const mcp_json::Value* position =
                matches->AsArray()[0].Find("timeline_position");
            const mcp_json::Value* value =
                position ? position->Find("value") : nullptr;
            int64_t frames = 0;
            Check(value != nullptr && value->AsInt64(frames) && frames == 10,
                  "source frame 105 is at timeline frame 10, not at 105");
        }
        Check(call("locate_source_frame",
                   R"({"media_id":"01K30000000000000000000001",)"
                   R"("source_frame":4000})")
                  .ok,
              "a frame that is not on the timeline is a fact, not an error");

        // The cut takes it too, and lands where locate_source_frame said.
        Check(call("split_clip", R"({"clip_id":"01K30000000000000000000003",)"
                                 R"("source_frame":105})")
                  .ok,
              "split_clip cuts on a source frame");
        const DocumentTrack* track =
            backend.CurrentDocument().FindTrack("01K30000000000000000000002");
        Check(track != nullptr && track->clips.size() == 3,
              "the clip is actually split in two");
        if (track != nullptr && track->clips.size() == 3) {
            Check(track->clips[1].timeline_in == RationalTime{10, 25} &&
                      track->clips[1].source_in == RationalTime{105, 25},
                  "the cut lands on the frame that was named, in both "
                  "domains");
        }
        EditError undoError = EditError::None;
        std::string undoMessage;
        backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                           undoError, undoMessage);

        Check(!call("split_clip", R"({"clip_id":"01K30000000000000000000003",)"
                                  R"("source_frame":105,"timeline_position":)"
                                  R"({"value":10,"rate":25}})")
                   .ok,
              "naming both a source frame and a timeline position is refused "
              "rather than resolved by precedence");
        Check(!call("split_clip", R"({"clip_id":"01K30000000000000000000003"})")
                   .ok,
              "and naming neither is refused too");
        const McpToolCallOutcome outOfBoundsSplit =
            call("split_clip", R"({"clip_id":"01K30000000000000000000003",)"
                               R"("source_frame":4000})");
        Check(
            !outOfBoundsSplit.ok &&
                outOfBoundsSplit.error_name == "InvalidOperation" &&
                outOfBoundsSplit.message.find("[101, 109]") !=
                    std::string::npos &&
                outOfBoundsSplit.message.find("got 4000") != std::string::npos,
            "a source-frame cut outside the clip is InvalidOperation with "
            "its actual bounds and value: " +
                outOfBoundsSplit.message);

        // A trim reads the same address, with the tail inclusive.
        Check(call("trim_clip", R"({"clip_id":"01K30000000000000000000003",)"
                                R"("edge":"Head","source_frame":103})")
                  .ok,
              "trim_clip enters on a source frame");
        const DocumentClip* trimmed =
            backend.CurrentDocument().FindClip("01K30000000000000000000003");
        Check(trimmed != nullptr && trimmed->source_in == RationalTime{103, 25},
              "and the clip starts on exactly that frame");
        Check(
            !call("trim_clip", R"({"clip_id":"01K30000000000000000000003",)"
                               R"("edge":"Head","delta":{"value":1,"rate":25},)"
                               R"("source_frame":103})")
                 .ok,
            "a delta and a source frame together are refused");
    }

    // QC-2026-09 A2 -- tighten_pauses. What matters at this layer is that the
    // clip's linked sound is carried by the same cut and that the whole thing
    // is one undoable event: the arithmetic itself is pinned in
    // tests/pause_tightening_tests.cc.
    {
        Document pauses;
        pauses.sources = {
            {"01K30000000000000000000001", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture;
        picture.id = "01K30000000000000000000010";
        picture.source_id = "01K30000000000000000000001";
        picture.source_in = {0, 25};
        picture.duration = {75, 25};
        picture.timeline_in = {0, 25};
        picture.link_group_id = "01K30000000000000000000012";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000011";
        pauses.sequence.tracks = {
            {"01K30000000000000000000013", "video", 0, {picture}},
            {"01K30000000000000000000014", "audio", 1, {sound}},
        };

        // Une seconde de parole, une de silence, une de parole.
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000001";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        for (int index = 0; index < 150; ++index)
            envelope.levels.push_back(index >= 50 && index < 100 ? 20000
                                                                 : 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        McpToolRegistry registry;
        InMemoryBackend backend(pauses);
        backend.SetSourceSpeechOnset(envelope);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "tighten_pauses arguments parse: " + parseFailure);
            return registry.Call(backend, "tighten_pauses", arguments);
        };

        const McpToolCallOutcome outcome =
            call(R"({"clip_id":"01K30000000000000000000010"})");
        Check(outcome.ok, "tighten_pauses succeeds: " + outcome.message);
        mcp_json::Value result;
        std::string resultError;
        Check(mcp_json::Value::Parse(outcome.result_json, result, resultError),
              "its result is JSON: " + resultError);
        const mcp_json::Value* applied = result.Find("applied");
        Check(applied != nullptr && applied->IsBool() && applied->AsBool(),
              "a clip with a one-second hole is actually tightened");
        const mcp_json::Value* alsoCut = result.Find("also_cut");
        Check(alsoCut != nullptr && alsoCut->IsArray() &&
                  alsoCut->AsArray().size() == 1 &&
                  alsoCut->AsArray()[0].AsString() ==
                      "01K30000000000000000000011",
              "the linked sound is cut by the same operation, not left behind");
        const DocumentTrack* video =
            backend.CurrentDocument().FindTrack("01K30000000000000000000013");
        const DocumentTrack* audio =
            backend.CurrentDocument().FindTrack("01K30000000000000000000014");
        Check(video != nullptr && audio != nullptr &&
                  video->clips.size() == 2 && audio->clips.size() == 2,
              "both tracks are split into the fragments that remain");

        EditError undoError = EditError::None;
        std::string undoMessage;
        Check(
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage),
            "one gesture is one undo step: " + undoMessage);

        // Nothing to close is a success, and says so rather than failing.
        SpeechOnsetReport unbroken;
        unbroken.media_id = "01K30000000000000000000001";
        unbroken.windows_per_second = 50;
        unbroken.decode_sample_rate = 16000;
        unbroken.levels.assign(150, 200000);
        unbroken.speech_level = SpeechLevelPercentile(unbroken.levels, 90);
        unbroken.noise_floor = SpeechLevelPercentile(unbroken.levels, 5);
        InMemoryBackend tight(pauses);
        tight.SetSourceSpeechOnset(unbroken);
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000010"})",
                               arguments, parseFailure);
        const McpToolCallOutcome nothing =
            registry.Call(tight, "tighten_pauses", arguments);
        Check(nothing.ok, "an already tight clip is not an error");
        mcp_json::Value nothingResult;
        mcp_json::Value::Parse(nothing.result_json, nothingResult, resultError);
        const mcp_json::Value* nothingApplied = nothingResult.Find("applied");
        Check(nothingApplied != nullptr && !nothingApplied->AsBool(),
              "and reports that it changed nothing");

        Check(!call(R"({"clip_id":"01K30000000000000000000010","keep":6})").ok,
              "a misspelled argument is refused rather than ignored");

        InMemoryBackend blind(pauses);
        const McpToolCallOutcome missing =
            registry.Call(blind, "tighten_pauses", arguments);
        Check(!missing.ok &&
                  missing.message.find("speech envelope") != std::string::npos,
              "a source with no envelope reports what is missing");
    }

    // B4 -- boundary and junction air are intent-level MCP tools backed by
    // one serialized operation. The pure arithmetic and four-pass regression
    // live in boundary_air_tests; this pins ID parsing, cache access, linked
    // A/V propagation, sync-lock defaults, and the public result envelope.
    {
        Document boundaries;
        boundaries.sources = {
            {"01K30000000000000000000201", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture;
        picture.id = "01K30000000000000000000210";
        picture.source_id = "01K30000000000000000000201";
        picture.source_in = {0, 25};
        picture.duration = {50, 25};
        picture.timeline_in = {0, 25};
        picture.link_group_id = "01K30000000000000000000220";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000211";
        DocumentClip follower = picture;
        follower.id = "01K30000000000000000000212";
        follower.link_group_id.clear();
        follower.source_in = {100, 25};
        follower.duration = {10, 25};
        follower.timeline_in = {50, 25};
        boundaries.sequence.tracks = {
            {"01K30000000000000000000230", "video", 0, {picture}},
            {"01K30000000000000000000231", "audio", 1, {sound}},
            {"01K30000000000000000000232", "video", 2, {follower}},
        };
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000201";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        envelope.levels.insert(envelope.levels.end(), 20, 20000);
        envelope.levels.insert(envelope.levels.end(), 50, 200000);
        envelope.levels.insert(envelope.levels.end(), 30, 20000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        McpToolRegistry registry;
        InMemoryBackend backend(boundaries);
        backend.SetSourceSpeechOnset(envelope);
        mcp_json::Value arguments;
        std::string parseError;
        mcp_json::Value::Parse(R"({"clip_id":"01K30000000000000000000210"})",
                               arguments, parseError);
        const McpToolCallOutcome outcome =
            registry.Call(backend, "trim_boundary_air", arguments);
        Check(outcome.ok, "trim_boundary_air succeeds: " + outcome.message);
        mcp_json::Value result;
        Check(mcp_json::Value::Parse(outcome.result_json, result, parseError),
              "trim_boundary_air returns JSON: " + parseError);
        const mcp_json::Value* alsoCut = result.Find("also_cut");
        const mcp_json::Value* syncTracks = result.Find("sync_track_ids");
        Check(alsoCut && alsoCut->IsArray() && alsoCut->AsArray().size() == 1 &&
                  alsoCut->AsArray()[0].AsString() == sound.id,
              "trim_boundary_air reports the linked sound it carried");
        Check(syncTracks && syncTracks->IsArray() &&
                  syncTracks->AsArray().size() == 1 &&
                  syncTracks->AsArray()[0].AsString() ==
                      "01K30000000000000000000232",
              "trim_boundary_air derives the other sync-locked track");
        Check(
            backend.CurrentDocument().FindClip(picture.id)->source_in ==
                    RationalTime{7, 25} &&
                backend.CurrentDocument().FindClip(sound.id)->source_in ==
                    RationalTime{7, 25} &&
                backend.CurrentDocument().FindClip(follower.id)->timeline_in ==
                    RationalTime{31, 25},
            "MCP reaches the atomic engine operation on every track");
    }

    {
        Document junction;
        junction.sources = {
            {"01K30000000000000000000301", "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip left;
        left.id = "01K30000000000000000000310";
        left.source_id = "01K30000000000000000000301";
        left.source_in = {0, 25};
        left.duration = {32, 25};
        left.timeline_in = {0, 25};
        DocumentClip right = left;
        right.id = "01K30000000000000000000311";
        right.source_in = {32, 25};
        right.timeline_in = {32, 25};
        junction.sequence.tracks = {
            {"01K30000000000000000000320", "audio", 0, {left, right}},
        };
        SpeechOnsetReport envelope;
        envelope.media_id = "01K30000000000000000000301";
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        envelope.levels.insert(envelope.levels.end(), 50, 200000);
        envelope.levels.insert(envelope.levels.end(), 28, 20000);
        envelope.levels.insert(envelope.levels.end(), 50, 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);

        McpToolRegistry registry;
        InMemoryBackend backend(junction);
        backend.SetSourceSpeechOnset(envelope);
        mcp_json::Value arguments;
        std::string parseError;
        mcp_json::Value::Parse(
            R"({"left_clip_id":"01K30000000000000000000310","right_clip_id":"01K30000000000000000000311","keep_frames":2})",
            arguments, parseError);
        const McpToolCallOutcome outcome =
            registry.Call(backend, "close_junction_air", arguments);
        Check(outcome.ok, "close_junction_air succeeds: " + outcome.message);
        const DocumentClip* newLeft =
            backend.CurrentDocument().FindClip(left.id);
        const DocumentClip* newRight =
            backend.CurrentDocument().FindClip(right.id);
        Check(newLeft && newRight &&
                  newLeft->duration == RationalTime{27, 25} &&
                  newRight->source_in == RationalTime{37, 25} &&
                  newRight->timeline_in == RationalTime{27, 25},
              "close_junction_air trims both sides in its MCP call");
    }

    // QC-2026-09 (A1) -- align_transcript. The correction only reaches the
    // tools that cut on words when `apply` does, so what this pins is that
    // the flag survives the dispatcher rather than defaulting either way by
    // accident.
    {
        McpToolRegistry registry;
        InMemoryBackend backend(fixture);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "align_transcript arguments parse: " + parseFailure);
            return registry.Call(backend, "align_transcript", arguments);
        };

        Check(call(R"({})").ok, "align_transcript takes no required argument");
        Check(
            backend.alignment_request.seen && !backend.alignment_request.apply,
            "reading what the signal contradicts writes nothing by default");
        Check(call(R"({"apply":true})").ok, "the write form succeeds");
        Check(backend.alignment_request.apply,
              "apply reaches the engine rather than being swallowed");
        Check(!call(R"({"aply":true})").ok,
              "a misspelled argument is refused rather than ignored");

        class BareBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool AlignSourceTranscripts(bool, std::string&,
                                        std::string& message) override {
                return McpBackend::AlignSourceTranscripts(false, message,
                                                          message);
            }
        };
        BareBackend bare(fixture);
        mcp_json::Value empty = mcp_json::Value::MakeObject();
        const McpToolCallOutcome missing =
            registry.Call(bare, "align_transcript", empty);
        Check(!missing.ok &&
                  missing.message.find("transcript cache") != std::string::npos,
              "a backend without a transcript cache reports the gap");
    }

    // A backend that never implements it must say so rather than pretend.
    {
        class BareBackend : public InMemoryBackend {
        public:
            using InMemoryBackend::InMemoryBackend;
            bool TranscribeSources(const std::vector<Ulid>&, const std::string&,
                                   bool, bool, std::string&,
                                   std::string& message) override {
                return McpBackend::TranscribeSources({}, std::string(), false,
                                                     false, message, message);
            }
        };
        McpToolRegistry registry;
        BareBackend bare(fixture);
        mcp_json::Value arguments;
        std::string parseFailure;
        mcp_json::Value::Parse(R"({"media_id":"01K30000000000000000000001"})",
                               arguments, parseFailure);
        const McpToolCallOutcome outcome =
            registry.Call(bare, "transcribe_media", arguments);
        Check(
            !outcome.ok && outcome.message.find("cannot run a transcription") !=
                               std::string::npos,
            "a backend without transcription reports the gap explicitly");
    }

    // QC-2026-09 B5 -- ripple_trim carries the linked picture/sound pair by
    // default, while an explicit linked_clip_ids array is an intentional
    // subset. A1 is deliberately left outside the default set so the tool
    // can warn that an omitted sync_track_ids would break V1/V2/A1 alignment.
    {
        const Ulid sourceId = "01K30000000000000000000001";
        const Ulid groupId = "01K30000000000000000000010";
        Document ripple;
        ripple.sources = {{sourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip v1{
            "01K30000000000000000000011", sourceId, {0, 25}, {10, 25}, {0, 25}};
        v1.link_group_id = groupId;
        v1.sync_anchor_clip_id = v1.id;
        DocumentClip v2 = v1;
        v2.id = "01K30000000000000000000012";
        v2.sync_anchor_clip_id = v1.id;
        DocumentClip v1After{"01K30000000000000000000013",
                             sourceId,
                             {20, 25},
                             {10, 25},
                             {10, 25}};
        DocumentClip v2After = v1After;
        v2After.id = "01K30000000000000000000014";
        DocumentClip a1{"01K30000000000000000000015",
                        sourceId,
                        {40, 25},
                        {10, 25},
                        {10, 25}};
        const Ulid v1TrackId = "01K30000000000000000000016";
        const Ulid v2TrackId = "01K30000000000000000000017";
        const Ulid a1TrackId = "01K30000000000000000000018";
        ripple.sequence.tracks = {
            {v1TrackId, "video", 0, {v1, v1After}},
            {v2TrackId, "video", 1, {v2, v2After}},
            {a1TrackId, "audio", 2, {a1}},
        };

        McpToolRegistry registry;
        InMemoryBackend backend(ripple);
        const auto call = [&](const std::string& argumentsJson) {
            mcp_json::Value arguments;
            std::string parseFailure;
            Check(
                mcp_json::Value::Parse(argumentsJson, arguments, parseFailure),
                "ripple_trim arguments parse: " + parseFailure);
            return registry.Call(backend, "ripple_trim", arguments);
        };

        const std::string before = backend.CurrentDocument().SaveToString();
        const McpToolCallOutcome carried =
            call(R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                 R"("delta":{"value":5,"rate":25}})");
        Check(carried.ok, "ripple_trim carries the linked group by default: " +
                              carried.message);
        const Document& carriedDocument = backend.CurrentDocument();
        Check(
            carriedDocument.FindClip("01K30000000000000000000011")->duration ==
                    RationalTime{15, 25} &&
                carriedDocument.FindClip("01K30000000000000000000012")
                        ->duration == RationalTime{15, 25},
            "default ripple trims both V1 and V2");
        Check(carriedDocument.FindClip("01K30000000000000000000013")
                          ->timeline_in == RationalTime{15, 25} &&
                  carriedDocument.FindClip("01K30000000000000000000014")
                          ->timeline_in == RationalTime{15, 25},
              "default ripple keeps downstream V1/V2 aligned");
        Check(carriedDocument.FindClip("01K30000000000000000000015")
                      ->timeline_in == RationalTime{10, 25},
              "unlisted A1 is not shifted without sync_track_ids");
        mcp_json::Value carriedResult;
        std::string resultError;
        Check(mcp_json::Value::Parse(carried.result_json, carriedResult,
                                     resultError),
              "ripple_trim result is JSON: " + resultError);
        const mcp_json::Value* alsoCut = carriedResult.Find("also_cut");
        Check(alsoCut && alsoCut->IsArray() && alsoCut->AsArray().size() == 1 &&
                  alsoCut->AsArray()[0].AsString() == v2.id,
              "ripple_trim reports the linked partner in also_cut");
        Check(carriedResult.Find("warning") != nullptr &&
                  carriedResult.Find("warning_track_ids") != nullptr,
              "omitting sync_track_ids warns about downstream A1");
        EditError undoError = EditError::None;
        std::string undoMessage;
        Check(
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage) &&
                backend.CurrentDocument().SaveToString() == before,
            "default ripple undo restores V1/V2/A1 byte-identically");

        InMemoryBackend subset(ripple);
        const McpToolCallOutcome explicitSubset = [&] {
            mcp_json::Value arguments;
            std::string parseFailure;
            mcp_json::Value::Parse(
                R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                R"("delta":{"value":5,"rate":25},"linked_clip_ids":[]})",
                arguments, parseFailure);
            return registry.Call(subset, "ripple_trim", arguments);
        }();
        Check(explicitSubset.ok &&
                  subset.CurrentDocument()
                          .FindClip("01K30000000000000000000011")
                          ->duration == RationalTime{15, 25} &&
                  subset.CurrentDocument()
                          .FindClip("01K30000000000000000000012")
                          ->duration == RationalTime{10, 25},
              "explicit empty linked_clip_ids preserves a subset");
        mcp_json::Value subsetResult;
        Check(mcp_json::Value::Parse(explicitSubset.result_json, subsetResult,
                                     resultError) &&
                  subsetResult.Find("also_cut") &&
                  subsetResult.Find("also_cut")->AsArray().empty(),
              "explicit subset reports no linked partner cut");

        InMemoryBackend aligned(ripple);
        mcp_json::Value alignedArguments;
        std::string alignedParseError;
        Check(mcp_json::Value::Parse(
                  R"({"clip_id":"01K30000000000000000000011","edge":"Tail",)"
                  R"("delta":{"value":5,"rate":25},)"
                  R"("sync_track_ids":["01K30000000000000000000018"]})",
                  alignedArguments, alignedParseError),
              "aligned ripple arguments parse: " + alignedParseError);
        const McpToolCallOutcome alignment =
            registry.Call(aligned, "ripple_trim", alignedArguments);
        Check(alignment.ok && aligned.CurrentDocument()
                                      .FindClip("01K30000000000000000000015")
                                      ->timeline_in == RationalTime{15, 25},
              "explicit sync_track_ids keeps V1/V2/A1 aligned");
        mcp_json::Value alignmentResult;
        Check(mcp_json::Value::Parse(alignment.result_json, alignmentResult,
                                     resultError) &&
                  alignmentResult.Find("warning") == nullptr,
              "explicit synchronization suppresses the omission warning");
    }

    // QC-2026-09 C1 -- insert_clip only used to ripple the track it targets;
    // an A/V-linked partner living on another track was silently left
    // behind. sync_track_ids closes that gap the same way remove_clip's
    // does, with the same omission warning as ripple_trim above.
    {
        const Ulid sourceId = "01K30000000000000000000031";
        Document doc;
        doc.sources = {{sourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip v1{
            "01K30000000000000000000032", sourceId, {0, 25}, {10, 25}, {0, 25}};
        DocumentClip a1{
            "01K30000000000000000000033", sourceId, {0, 25}, {10, 25}, {0, 25}};
        const Ulid v1TrackId = "01K30000000000000000000034";
        const Ulid a1TrackId = "01K30000000000000000000035";
        doc.sequence.tracks = {
            {v1TrackId, "video", 0, {v1}},
            {a1TrackId, "audio", 1, {a1}},
        };
        const std::string insertArguments =
            R"({"track_id":")" + v1TrackId + R"(","source_id":")" + sourceId +
            R"(","source_in":{"value":100,"rate":25},)"
            R"("duration":{"value":5,"rate":25},)"
            R"("timeline_in":{"value":0,"rate":25})";

        McpToolRegistry registry;
        InMemoryBackend backend(doc);
        const std::string before = backend.CurrentDocument().SaveToString();
        std::string parseFailure;
        const McpToolCallOutcome omitted = [&] {
            mcp_json::Value arguments;
            Check(mcp_json::Value::Parse(insertArguments + "}", arguments,
                                         parseFailure),
                  "insert_clip arguments parse: " + parseFailure);
            return registry.Call(backend, "insert_clip", arguments);
        }();
        Check(omitted.ok, "insert_clip applies: " + omitted.message);
        Check(backend.CurrentDocument().FindClip(a1.id)->timeline_in ==
                  RationalTime{0, 25},
              "unlisted A1 is not shifted without sync_track_ids");
        mcp_json::Value omittedResult;
        std::string resultError;
        Check(mcp_json::Value::Parse(omitted.result_json, omittedResult,
                                     resultError),
              "insert_clip result is JSON: " + resultError);
        Check(omittedResult.Find("warning") != nullptr &&
                  omittedResult.Find("warning_track_ids") != nullptr,
              "omitting sync_track_ids warns about downstream A1");
        EditError undoError = EditError::None;
        std::string undoMessage;
        Check(
            backend.Log().Undo(const_cast<Document&>(backend.CurrentDocument()),
                               undoError, undoMessage) &&
                backend.CurrentDocument().SaveToString() == before,
            "insert undo restores V1/A1 byte-identically");

        InMemoryBackend aligned(doc);
        const McpToolCallOutcome synced = [&] {
            mcp_json::Value arguments;
            Check(mcp_json::Value::Parse(insertArguments +
                                             R"(,"sync_track_ids":[")" +
                                             a1TrackId + R"("]})",
                                         arguments, parseFailure),
                  "synced insert_clip arguments parse: " + parseFailure);
            return registry.Call(aligned, "insert_clip", arguments);
        }();
        Check(synced.ok, "synced insert_clip applies: " + synced.message);
        Check(aligned.CurrentDocument().FindClip(a1.id)->timeline_in ==
                  RationalTime{5, 25},
              "explicit sync_track_ids keeps A1 aligned with the ripple");
        mcp_json::Value syncedResult;
        Check(mcp_json::Value::Parse(synced.result_json, syncedResult,
                                     resultError) &&
                  syncedResult.Find("warning") == nullptr,
              "explicit synchronization suppresses the omission warning");
        Check(
            aligned.Log().Undo(const_cast<Document&>(aligned.CurrentDocument()),
                               undoError, undoMessage) &&
                aligned.CurrentDocument().SaveToString() == before,
            "synced insert undo restores V1/A1 byte-identically");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All MCP tool tests passed\n";
    return 0;
}

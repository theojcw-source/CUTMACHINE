#include "McpProjectBackend.h"

#include "Cli.h"
#include "FrameCapture.h"
#include "InterviewShort.h"
#include "Json.h"
#include "Project.h"
#include "ProjectStorage.h"

namespace {

// Cli.cc's headless commands each return `{"ok":true,...}` or
// `{"ok":false,"error":"<EditErrorName>","detail":"..."}`. Unpack that
// shared shape once instead of duplicating the parse in every method below.
bool ParseCommandResult(const std::string& output, bool commandSucceeded,
                        std::string& resultJson, std::string& errorName,
                        std::string& message) {
    mcp_json::Value value;
    std::string parseError;
    if (!mcp_json::Value::Parse(output, value, parseError)) {
        errorName = "IoError";
        message = "malformed command output: " + parseError;
        return false;
    }
    if (!commandSucceeded) {
        const mcp_json::Value* errorField = value.Find("error");
        const mcp_json::Value* detailField = value.Find("detail");
        errorName = (errorField && errorField->IsString())
                        ? errorField->AsString()
                        : "InvalidOperation";
        message = (detailField && detailField->IsString())
                      ? detailField->AsString()
                      : "operation failed";
        return false;
    }
    resultJson = output;
    return true;
}

}  // namespace

McpProjectBackend::McpProjectBackend(std::string projectPath,
                                     bool requireExplicitTimeline)
    : project_path_(std::move(projectPath)),
      require_explicit_timeline_(requireExplicitTimeline) {}

bool McpProjectBackend::SelectTimelineForEdit(const std::string& timelineId,
                                              std::string& errorName,
                                              std::string& message) {
    if (require_explicit_timeline_ && timelineId.empty()) {
        errorName = "TimelineRequired";
        message = "strict timeline editing requires an explicit timeline_id";
        return false;
    }
    Project project;
    if (!LoadStoredProject(project_path_, project, message)) {
        errorName = "IoError";
        return false;
    }
    selected_timeline_id_ =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selected_timeline_id_)) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + selected_timeline_id_ + "'";
        selected_timeline_id_.clear();
        return false;
    }
    return true;
}

bool McpProjectBackend::SnapshotDocument(Document& document,
                                         std::string& message) {
    Project project;
    if (!LoadStoredProject(project_path_, project, message)) return false;
    document = selected_timeline_id_.empty()
                   ? project.MakeActiveDocument()
                   : project.MakeDocument(selected_timeline_id_);
    return true;
}

bool McpProjectBackend::ApplyOperation(Operation operation,
                                       std::string& resultJson,
                                       std::string& errorName,
                                       std::string& message) {
    const std::string operationJson = SerializeOperation(operation);
    std::string output;
    const int result = ApplyOperationCommand(project_path_, operationJson,
                                             output, selected_timeline_id_);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::ApplyProjectEdit(ProjectOperation operation,
                                         std::string& resultJson,
                                         std::string& errorName,
                                         std::string& message) {
    std::string output;
    const int result = ApplyProjectOperationCommand(
        project_path_, SerializeProjectOperation(operation), output);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::ReadTimelineTranscript(std::string& json,
                                               std::string& message) {
    Project project;
    if (!LoadStoredProject(project_path_, project, message)) return false;
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    return DescribeTimelineTranscriptForAgent(
        selected_timeline_id_.empty()
            ? project.MakeActiveDocument()
            : project.MakeDocument(selected_timeline_id_),
        projectPath.parent_path() / ".cutmachine" / "transcripts", json,
        message);
}

bool McpProjectBackend::ReadSourceTranscript(const Ulid& sourceId,
                                             Transcript& transcript,
                                             std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "transcripts" /
                                       (sourceId + ".json");
    return LoadAudioTranscript(path.string(), transcript, message);
}

bool McpProjectBackend::ReadSourceShotQuality(const Ulid& sourceId,
                                              ShotQualityReport& report,
                                              std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "shotquality" /
                                       (sourceId + ".json");
    return LoadShotQuality(path.string(), report, message);
}

bool McpProjectBackend::ReadSourceSpeechOnset(const Ulid& sourceId,
                                              SpeechOnsetReport& report,
                                              std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "speechonset" /
                                       (sourceId + ".json");
    return LoadSpeechOnset(path.string(), report, message);
}

bool McpProjectBackend::AnalyzeSourceSpeechOnset(
    const Ulid& sourceId, const SpeechOnsetSettings& settings,
    std::string& resultJson, std::string& message) {
    std::string output;
    const int result =
        AnalyzeSpeechOnsetCommand(project_path_, sourceId, output, settings);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::AlignSourceTranscripts(bool apply,
                                               std::string& resultJson,
                                               std::string& message) {
    std::string output;
    const int result = AlignTranscriptsCommand(project_path_, apply, output);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::AnalyzeSourceShotQuality(const Ulid& sourceId,
                                                 std::string& resultJson,
                                                 std::string& message) {
    // Same command `--shot-quality` runs, for the same reason ApplyOperation
    // reuses ApplyOperationCommand: one analysis path, not two.
    std::string output;
    const int result =
        AnalyzeShotQualityCommand(project_path_, sourceId, output);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::TranscribeSources(const std::vector<Ulid>& sourceIds,
                                          const std::string& language,
                                          bool verbatim, bool includeSilent,
                                          std::string& resultJson,
                                          std::string& message) {
    // Same command `--transcribe` runs. The empty model path is deliberate:
    // it resolves the locally configured model, which is the only form a
    // caller that is not a human typing a path can use.
    std::string output;
    const int result =
        TranscribeMediaCommand(project_path_, sourceIds, "", language, verbatim,
                               includeSilent, output);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::TranscribeTimeline(const std::string& timelineId,
                                           const std::string& language,
                                           bool verbatim,
                                           std::string& resultJson,
                                           std::string& message) {
    std::string output;
    const int result = TranscribeTimelineCommand(project_path_, timelineId,
                                                 language, verbatim, output);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::CaptureSourceFrame(const Ulid& sourceId,
                                           const RationalTime& time,
                                           std::string& jpegBytes,
                                           std::string& message) {
    Project project;
    if (!LoadStoredProject(project_path_, project, message)) return false;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == sourceId; });
    if (media == project.rushes.end()) {
        message = "unknown media_id '" + sourceId + "'";
        return false;
    }
    if (!media->has_video) {
        message = "media_id '" + sourceId +
                  "' is audio-only; read_frame requires a video stream";
        return false;
    }
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path = std::filesystem::absolute(project_path_).parent_path() / path;
    }
    return ::CaptureSourceFrame(path.lexically_normal().string(), time,
                                FrameCaptureSettings{}, jpegBytes, message);
}

bool McpProjectBackend::Undo(std::string& resultJson, std::string& errorName,
                             std::string& message) {
    std::string output;
    const int result =
        UndoOperationCommand(project_path_, output, selected_timeline_id_);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::Redo(std::string& resultJson, std::string& errorName,
                             std::string& message) {
    std::string output;
    const int result =
        RedoOperationCommand(project_path_, output, selected_timeline_id_);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::Describe(std::string& json, std::string& message) {
    std::string output;
    const int result = DescribeCommand(project_path_, output);
    if (result == 0) {
        json = output;
        return true;
    }
    std::string errorName;
    std::string detail;
    ParseCommandResult(output, false, json, errorName, detail);
    message = errorName + ": " + detail;
    return false;
}

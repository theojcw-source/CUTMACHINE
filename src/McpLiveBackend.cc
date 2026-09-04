#include "McpLiveBackend.h"

#include "Cli.h"
#include "Operations.h"

McpLiveBackend::McpLiveBackend(Document& document, EditLog& editLog,
                               std::function<void()> onApplied,
                               ProjectApplyCallback applyProject,
                               ProjectDescribeCallback describeProject,
                               TranscriptCallback readTranscript,
                               SourceTranscriptCallback readSourceTranscript,
                               SourceShotQualityCallback readSourceShotQuality,
                               AnalyzeShotQualityCallback analyzeShotQuality,
                               CaptureFrameCallback captureFrame)
    : document_(document),
      edit_log_(editLog),
      on_applied_(std::move(onApplied)),
      apply_project_(std::move(applyProject)),
      describe_project_(std::move(describeProject)),
      read_transcript_(std::move(readTranscript)),
      read_source_transcript_(std::move(readSourceTranscript)),
      read_source_shot_quality_(std::move(readSourceShotQuality)),
      analyze_shot_quality_(std::move(analyzeShotQuality)),
      capture_frame_(std::move(captureFrame)) {}

bool McpLiveBackend::SnapshotDocument(Document& document, std::string&) {
    document = document_;
    return true;
}

bool McpLiveBackend::ApplyOperation(Operation operation,
                                    std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    EditError error = EditError::None;
    if (!edit_log_.Apply(document_, std::move(operation), error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    resultJson = "{\"ok\":true}";
    if (on_applied_) on_applied_();
    return true;
}

bool McpLiveBackend::ApplyProjectEdit(ProjectOperation operation,
                                      std::string& resultJson,
                                      std::string& errorName,
                                      std::string& message) {
    if (!apply_project_) {
        errorName = "UnsupportedOperation";
        message = "the live backend cannot create project timelines";
        return false;
    }
    return apply_project_(std::move(operation), resultJson, errorName, message);
}

bool McpLiveBackend::ReadTimelineTranscript(std::string& json,
                                            std::string& message) {
    if (!read_transcript_) {
        message = "the live backend has no transcript reader";
        return false;
    }
    return read_transcript_(json, message);
}

bool McpLiveBackend::ReadSourceTranscript(const Ulid& sourceId,
                                          Transcript& transcript,
                                          std::string& message) {
    if (!read_source_transcript_) {
        message = "the live backend has no transcript reader";
        return false;
    }
    return read_source_transcript_(sourceId, transcript, message);
}

bool McpLiveBackend::ReadSourceShotQuality(const Ulid& sourceId,
                                           ShotQualityReport& report,
                                           std::string& message) {
    if (!read_source_shot_quality_) {
        message = "the live backend has no shot quality reader";
        return false;
    }
    return read_source_shot_quality_(sourceId, report, message);
}

bool McpLiveBackend::AnalyzeSourceShotQuality(const Ulid& sourceId,
                                              std::string& resultJson,
                                              std::string& message) {
    if (!analyze_shot_quality_) {
        message = "the live backend cannot run a shot quality analysis";
        return false;
    }
    return analyze_shot_quality_(sourceId, resultJson, message);
}

bool McpLiveBackend::CaptureSourceFrame(const Ulid& sourceId,
                                        const RationalTime& time,
                                        std::string& jpegBytes,
                                        std::string& message) {
    if (!capture_frame_) {
        message = "the live backend cannot render a frame";
        return false;
    }
    return capture_frame_(sourceId, time, jpegBytes, message);
}

bool McpLiveBackend::Undo(std::string& resultJson, std::string& errorName,
                          std::string& message) {
    EditError error = EditError::None;
    if (!edit_log_.Undo(document_, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    resultJson = "{\"ok\":true}";
    if (on_applied_) on_applied_();
    return true;
}

bool McpLiveBackend::Redo(std::string& resultJson, std::string& errorName,
                          std::string& message) {
    EditError error = EditError::None;
    if (!edit_log_.Redo(document_, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    resultJson = "{\"ok\":true}";
    if (on_applied_) on_applied_();
    return true;
}

bool McpLiveBackend::Describe(std::string& json, std::string& message) {
    if (describe_project_) return describe_project_(json, message);
    json = DescribeDocument(document_);
    return true;
}

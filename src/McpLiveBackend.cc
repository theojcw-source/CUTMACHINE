#include "McpLiveBackend.h"

#include "Cli.h"
#include "Operations.h"

McpLiveBackend::McpLiveBackend(Document& document, EditLog& editLog,
                               std::function<void()> onApplied,
                               ProjectApplyCallback applyProject,
                               TranscriptCallback readTranscript)
    : document_(document),
      edit_log_(editLog),
      on_applied_(std::move(onApplied)),
      apply_project_(std::move(applyProject)),
      read_transcript_(std::move(readTranscript)) {}

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

bool McpLiveBackend::Describe(std::string& json, std::string&) {
    json = DescribeDocument(document_);
    return true;
}

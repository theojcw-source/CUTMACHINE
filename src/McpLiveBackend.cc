#include "McpLiveBackend.h"

#include "Cli.h"
#include "Operations.h"

McpLiveBackend::McpLiveBackend(Document& document, EditLog& editLog,
                               std::function<void()> onApplied)
    : document_(document),
      edit_log_(editLog),
      on_applied_(std::move(onApplied)) {}

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

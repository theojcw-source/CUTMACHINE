#include "McpProjectBackend.h"

#include "Cli.h"
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

McpProjectBackend::McpProjectBackend(std::string projectPath)
    : project_path_(std::move(projectPath)) {}

bool McpProjectBackend::SnapshotDocument(Document& document,
                                         std::string& message) {
    Project project;
    if (!LoadStoredProject(project_path_, project, message)) return false;
    document = project.MakeActiveDocument();
    return true;
}

bool McpProjectBackend::ApplyOperation(Operation operation,
                                       std::string& resultJson,
                                       std::string& errorName,
                                       std::string& message) {
    const std::string operationJson = SerializeOperation(operation);
    std::string output;
    const int result =
        ApplyOperationCommand(project_path_, operationJson, output);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::Undo(std::string& resultJson, std::string& errorName,
                             std::string& message) {
    std::string output;
    const int result = UndoOperationCommand(project_path_, output);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::Redo(std::string& resultJson, std::string& errorName,
                             std::string& message) {
    std::string output;
    const int result = RedoOperationCommand(project_path_, output);
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

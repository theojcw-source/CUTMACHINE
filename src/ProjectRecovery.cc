#include "ProjectRecovery.h"

#include "Json.h"
#include "Project.h"
#include "Ulid.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using mcp_json::Value;

constexpr char kEnvelopeFormat[] = "cutmachine-recovery";
constexpr int64_t kEnvelopeVersion = 1;

bool CheckProjectPath(const std::string& projectPath, std::string& error) {
    if (projectPath.empty()) {
        error = "project path must not be empty";
        return false;
    }
    error.clear();
    return true;
}

class TemporaryFile {
public:
    explicit TemporaryFile(fs::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (!committed_) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }

    const fs::path& path() const { return path_; }
    void Commit() { committed_ = true; }

private:
    fs::path path_;
    bool committed_ = false;
};

bool WriteFile(const fs::path& path, const std::string& contents,
               std::string& error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error =
            "unable to create recovery temporary file '" + path.string() + "'";
        return false;
    }
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        error =
            "unable to write recovery temporary file '" + path.string() + "'";
        return false;
    }
    output.close();
    if (!output) {
        error =
            "unable to close recovery temporary file '" + path.string() + "'";
        return false;
    }
    return true;
}

bool Synchronize(const fs::path& path, bool directory, std::string& error) {
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        error = "unable to open recovery path '" + path.string() +
                "' for synchronization: " + std::strerror(errno);
        return false;
    }
#ifdef F_FULLFSYNC
    const bool synchronized =
        directory
            ? fsync(descriptor) == 0
            : (fcntl(descriptor, F_FULLFSYNC) == 0 || fsync(descriptor) == 0);
#else
    const bool synchronized = fsync(descriptor) == 0;
#endif
    if (!synchronized) {
        const int savedError = errno;
        close(descriptor);
        error = "unable to synchronize recovery path '" + path.string() +
                "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        error = "unable to close recovery path '" + path.string() +
                "': " + std::strerror(errno);
        return false;
    }
    return true;
}

bool WriteCanonicalAutosave(const std::string& projectPath,
                            const std::string& contents, std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    const fs::path autosave(ProjectRecovery::AutosavePath(projectPath));
    const fs::path temporaryPath =
        fs::path(autosave.string() + "." + GenerateUlid() + ".tmp");
    TemporaryFile temporary(temporaryPath);
    if (!WriteFile(temporary.path(), contents, error)) return false;
    if (!Synchronize(temporary.path(), false, error)) return false;
    std::error_code renameError;
    fs::rename(temporary.path(), autosave, renameError);
    if (renameError) {
        error = "unable to atomically replace autosave '" + autosave.string() +
                "': " + renameError.message();
        return false;
    }
    const fs::path parent =
        autosave.has_parent_path() ? autosave.parent_path() : fs::path(".");
    if (!Synchronize(parent, true, error)) return false;
    temporary.Commit();
    error.clear();
    return true;
}

bool HasExactKeys(const Value& value,
                  std::initializer_list<const char*> expected,
                  const std::string& context, std::string& error) {
    if (!value.IsObject()) {
        error = context + " must be an object";
        return false;
    }
    if (value.AsObject().size() != expected.size()) {
        error = context + " has unexpected or missing fields";
        return false;
    }
    for (const char* key : expected) {
        if (!value.Find(key)) {
            error = context + " is missing field '" + key + "'";
            return false;
        }
    }
    return true;
}

bool ValidateTimelineLogs(const Project& project,
                          const std::map<std::string, EditLog>& timelineLogs,
                          std::string& error) {
    if (timelineLogs.size() != project.timelines.size()) {
        error =
            "recovery envelope must contain exactly one edit log per "
            "timeline";
        return false;
    }
    for (const DocumentSequence& timeline : project.timelines) {
        const auto found = timelineLogs.find(timeline.id);
        if (found == timelineLogs.end()) {
            error = "recovery envelope is missing edit log for timeline '" +
                    timeline.id + "'";
            return false;
        }
        EditLog decoded;
        EditError editError = EditError::None;
        std::string message;
        if (!EditLog::Deserialize(found->second.Serialize(), decoded, editError,
                                  message)) {
            error = "invalid edit log for timeline '" + timeline.id +
                    "': " + message;
            return false;
        }
    }
    for (const auto& entry : timelineLogs) {
        if (!project.FindTimeline(entry.first)) {
            error =
                "recovery envelope contains edit log for unknown "
                "timeline '" +
                entry.first + "'";
            return false;
        }
    }
    error.clear();
    return true;
}

bool SerializeEnvelope(const Project& project,
                       const std::map<std::string, EditLog>& timelineLogs,
                       const ProjectEditLog& projectLog, std::string& output,
                       std::string& error) {
    if (!project.Validate(error)) {
        error = "cannot autosave invalid project: " + error;
        return false;
    }
    if (!ValidateTimelineLogs(project, timelineLogs, error)) return false;

    ProjectEditLog decodedProjectLog;
    EditError editError = EditError::None;
    std::string message;
    if (!ProjectEditLog::Deserialize(projectLog.Serialize(), decodedProjectLog,
                                     editError, message)) {
        error = "invalid project edit log: " + message;
        return false;
    }

    Value root = Value::MakeObject();
    root.Set("autosave_format", Value::MakeString(kEnvelopeFormat));
    root.Set("autosave_version", Value::MakeInt(kEnvelopeVersion));
    root.Set("project", Value::MakeString(project.SaveToString()));
    Value logs = Value::MakeArray();
    for (const auto& entry : timelineLogs) {
        Value item = Value::MakeObject();
        item.Set("timeline_id", Value::MakeString(entry.first));
        item.Set("edit_log", Value::MakeString(entry.second.Serialize()));
        logs.Push(std::move(item));
    }
    root.Set("timeline_logs", std::move(logs));
    root.Set("project_edit_log", Value::MakeString(projectLog.Serialize()));
    output = root.Dump() + "\n";
    error.clear();
    return true;
}

bool DeserializeEnvelope(const std::string& contents, Project& projectOutput,
                         std::map<std::string, EditLog>& timelineLogsOutput,
                         ProjectEditLog& projectLogOutput, std::string& error) {
    Value root;
    if (!Value::Parse(contents, root, error)) {
        error = "invalid recovery envelope JSON: " + error;
        return false;
    }
    if (!HasExactKeys(root,
                      {"autosave_format", "autosave_version", "project",
                       "timeline_logs", "project_edit_log"},
                      "recovery envelope", error)) {
        return false;
    }

    const Value* format = root.Find("autosave_format");
    if (!format->IsString() || format->AsString() != kEnvelopeFormat) {
        error = "unsupported recovery envelope format";
        return false;
    }
    int64_t version = 0;
    const Value* versionValue = root.Find("autosave_version");
    if (!versionValue->AsInt64(version) || version != kEnvelopeVersion) {
        error = "unsupported recovery envelope version";
        return false;
    }
    const Value* projectValue = root.Find("project");
    if (!projectValue->IsString()) {
        error = "recovery envelope project must be a JSON string";
        return false;
    }
    Project project;
    std::string parseError;
    if (!Project::LoadFromString(projectValue->AsString(), project,
                                 parseError)) {
        error = "invalid project in recovery envelope: " + parseError;
        return false;
    }

    const Value* logsValue = root.Find("timeline_logs");
    if (!logsValue->IsArray()) {
        error = "recovery envelope timeline_logs must be an array";
        return false;
    }
    std::map<std::string, EditLog> timelineLogs;
    for (size_t index = 0; index < logsValue->AsArray().size(); ++index) {
        const Value& item = logsValue->AsArray()[index];
        const std::string context =
            "recovery envelope timeline_logs[" + std::to_string(index) + "]";
        if (!HasExactKeys(item, {"timeline_id", "edit_log"}, context, error)) {
            return false;
        }
        const Value* timelineId = item.Find("timeline_id");
        const Value* editLogValue = item.Find("edit_log");
        if (!timelineId->IsString() || !editLogValue->IsString()) {
            error = context + " fields must be JSON strings";
            return false;
        }
        EditLog editLog;
        EditError editError = EditError::None;
        std::string message;
        if (!EditLog::Deserialize(editLogValue->AsString(), editLog, editError,
                                  message)) {
            error = context + " contains an invalid edit log: " + message;
            return false;
        }
        if (!timelineLogs.emplace(timelineId->AsString(), std::move(editLog))
                 .second) {
            error = context + " duplicates timeline '" +
                    timelineId->AsString() + "'";
            return false;
        }
    }
    if (!ValidateTimelineLogs(project, timelineLogs, error)) return false;

    const Value* projectLogValue = root.Find("project_edit_log");
    if (!projectLogValue->IsString()) {
        error = "recovery envelope project_edit_log must be a JSON string";
        return false;
    }
    ProjectEditLog projectLog;
    EditError editError = EditError::None;
    std::string message;
    if (!ProjectEditLog::Deserialize(projectLogValue->AsString(), projectLog,
                                     editError, message)) {
        error = "invalid project edit log in recovery envelope: " + message;
        return false;
    }

    projectOutput = std::move(project);
    timelineLogsOutput = std::move(timelineLogs);
    projectLogOutput = std::move(projectLog);
    error.clear();
    return true;
}

bool InspectAutosaveFormat(const fs::path& autosave,
                           ProjectRecoveryFormat& format, std::string& error) {
    std::ifstream input(autosave, std::ios::binary);
    if (!input) {
        format = ProjectRecoveryFormat::Unknown;
        error = "unable to open autosave '" + autosave.string() + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        format = ProjectRecoveryFormat::Unknown;
        error = "unable to read autosave '" + autosave.string() + "'";
        return false;
    }

    Project project;
    std::map<std::string, EditLog> timelineLogs;
    ProjectEditLog projectLog;
    std::string envelopeError;
    if (DeserializeEnvelope(contents.str(), project, timelineLogs, projectLog,
                            envelopeError)) {
        format = ProjectRecoveryFormat::Envelope;
        error.clear();
        return true;
    }

    std::string projectError;
    if (Project::LoadFromString(contents.str(), project, projectError)) {
        format = ProjectRecoveryFormat::Project;
        error.clear();
        return true;
    }

    Document document;
    std::string documentError;
    if (Document::LoadFromString(contents.str(), document, documentError)) {
        format = ProjectRecoveryFormat::Document;
        error.clear();
        return true;
    }

    format = ProjectRecoveryFormat::Unknown;
    error = "not a valid recovery envelope (" + envelopeError + "), project (" +
            projectError + ") or document (" + documentError + ")";
    return false;
}

}  // namespace

std::string ProjectRecovery::AutosavePath(const std::string& projectPath) {
    if (projectPath.empty()) return {};
    return projectPath + ".autosave";
}

bool ProjectRecovery::WriteAutosave(const std::string& projectPath,
                                    const Document& document,
                                    std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    if (!document.Validate(error)) {
        error = "cannot autosave invalid document: " + error;
        return false;
    }

    return WriteCanonicalAutosave(projectPath, document.SaveToString(), error);
}

bool ProjectRecovery::WriteAutosave(const std::string& projectPath,
                                    const Project& project,
                                    std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    if (!project.Validate(error)) {
        error = "cannot autosave invalid project: " + error;
        return false;
    }
    return WriteCanonicalAutosave(projectPath, project.SaveToString(), error);
}

bool ProjectRecovery::WriteAutosave(
    const std::string& projectPath, const Project& project,
    const std::map<std::string, EditLog>& timelineLogs,
    const ProjectEditLog& projectLog, std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    std::string contents;
    if (!SerializeEnvelope(project, timelineLogs, projectLog, contents, error))
        return false;

    Project decodedProject;
    std::map<std::string, EditLog> decodedTimelineLogs;
    ProjectEditLog decodedProjectLog;
    if (!DeserializeEnvelope(contents, decodedProject, decodedTimelineLogs,
                             decodedProjectLog, error)) {
        error = "cannot autosave invalid recovery envelope: " + error;
        return false;
    }
    return WriteCanonicalAutosave(projectPath, contents, error);
}

ProjectRecoveryInfo ProjectRecovery::Inspect(const std::string& projectPath) {
    ProjectRecoveryInfo info;
    info.project_path = projectPath;
    info.autosave_path = AutosavePath(projectPath);
    if (!CheckProjectPath(projectPath, info.error)) {
        info.state = ProjectRecoveryState::Invalid;
        return info;
    }

    std::error_code statusError;
    const fs::file_status autosaveStatus =
        fs::status(info.autosave_path, statusError);
    if (statusError != std::errc::no_such_file_or_directory && statusError) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "unable to inspect autosave '" + info.autosave_path +
                     "': " + statusError.message();
        return info;
    }
    if (statusError == std::errc::no_such_file_or_directory ||
        !fs::exists(autosaveStatus)) {
        info.state = ProjectRecoveryState::None;
        return info;
    }
    if (!fs::is_regular_file(autosaveStatus)) {
        info.state = ProjectRecoveryState::Invalid;
        info.error =
            "autosave is not a regular file: '" + info.autosave_path + "'";
        return info;
    }

    if (!InspectAutosaveFormat(info.autosave_path, info.format, info.error)) {
        info.state = ProjectRecoveryState::Invalid;
        info.error =
            "invalid autosave '" + info.autosave_path + "': " + info.error;
        return info;
    }

    statusError.clear();
    const fs::file_status projectStatus = fs::status(projectPath, statusError);
    if (statusError != std::errc::no_such_file_or_directory && statusError) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "unable to inspect project '" + projectPath +
                     "': " + statusError.message();
        return info;
    }
    if (statusError == std::errc::no_such_file_or_directory ||
        !fs::exists(projectStatus)) {
        info.state = ProjectRecoveryState::Available;
        info.error.clear();
        return info;
    }
    if (!fs::is_regular_file(projectStatus)) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "project is not a regular file: '" + projectPath + "'";
        return info;
    }

    const fs::file_time_type autosaveTime =
        fs::last_write_time(info.autosave_path, statusError);
    if (statusError) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "unable to read autosave modification time: " +
                     statusError.message();
        return info;
    }
    const fs::file_time_type projectTime =
        fs::last_write_time(projectPath, statusError);
    if (statusError) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "unable to read project modification time: " +
                     statusError.message();
        return info;
    }

    info.state = autosaveTime > projectTime ? ProjectRecoveryState::Available
                                            : ProjectRecoveryState::Stale;
    info.error.clear();
    return info;
}

bool ProjectRecovery::LoadAutosave(const std::string& projectPath,
                                   Document& output, std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    Document recovered;
    if (!Document::Load(AutosavePath(projectPath), recovered, error)) {
        error = "unable to load autosave: " + error;
        return false;
    }
    output = std::move(recovered);
    error.clear();
    return true;
}

bool ProjectRecovery::LoadAutosave(const std::string& projectPath,
                                   Project& output, std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    const fs::path autosave(AutosavePath(projectPath));
    std::ifstream input(autosave, std::ios::binary);
    if (!input) {
        error = "unable to load project autosave: unable to open autosave '" +
                autosave.string() + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to load project autosave: unable to read autosave '" +
                autosave.string() + "'";
        return false;
    }

    Project recovered;
    std::map<std::string, EditLog> ignoredTimelineLogs;
    ProjectEditLog ignoredProjectLog;
    std::string envelopeError;
    if (DeserializeEnvelope(contents.str(), recovered, ignoredTimelineLogs,
                            ignoredProjectLog, envelopeError)) {
        output = std::move(recovered);
        error.clear();
        return true;
    }
    std::string projectError;
    if (!Project::LoadFromString(contents.str(), recovered, projectError)) {
        error =
            "unable to load project autosave: not a valid recovery "
            "envelope (" +
            envelopeError + ") or project (" + projectError + ")";
        return false;
    }
    output = std::move(recovered);
    error.clear();
    return true;
}

bool ProjectRecovery::LoadAutosave(
    const std::string& projectPath, Project& projectOutput,
    std::map<std::string, EditLog>& timelineLogsOutput,
    ProjectEditLog& projectLogOutput, std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    const fs::path autosave(AutosavePath(projectPath));
    std::ifstream input(autosave, std::ios::binary);
    if (!input) {
        error = "unable to load recovery envelope: unable to open autosave '" +
                autosave.string() + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to load recovery envelope: unable to read autosave '" +
                autosave.string() + "'";
        return false;
    }

    Project project;
    std::map<std::string, EditLog> timelineLogs;
    ProjectEditLog projectLog;
    std::string envelopeError;
    if (!DeserializeEnvelope(contents.str(), project, timelineLogs, projectLog,
                             envelopeError)) {
        std::string projectError;
        if (!Project::LoadFromString(contents.str(), project, projectError)) {
            error = "unable to load recovery envelope: not a valid envelope (" +
                    envelopeError + ") or legacy project (" + projectError +
                    ")";
            return false;
        }
        for (const DocumentSequence& timeline : project.timelines)
            timelineLogs.emplace(timeline.id, EditLog{});
    }

    projectOutput = std::move(project);
    timelineLogsOutput = std::move(timelineLogs);
    projectLogOutput = std::move(projectLog);
    error.clear();
    return true;
}

bool ProjectRecovery::DiscardAutosave(const std::string& projectPath,
                                      std::string& error) {
    if (!CheckProjectPath(projectPath, error)) return false;
    const fs::path autosave(AutosavePath(projectPath));
    std::error_code statusError;
    const fs::file_status status = fs::symlink_status(autosave, statusError);
    if (statusError != std::errc::no_such_file_or_directory && statusError) {
        error = "unable to inspect autosave before discard: " +
                statusError.message();
        return false;
    }
    if (statusError == std::errc::no_such_file_or_directory ||
        !fs::exists(status)) {
        error.clear();
        return true;
    }
    if (fs::is_directory(status)) {
        error = "refusing to discard autosave directory '" + autosave.string() +
                "'";
        return false;
    }
    std::error_code removeError;
    if (!fs::remove(autosave, removeError)) {
        error = "unable to discard autosave '" + autosave.string() + "'";
        if (removeError) error += ": " + removeError.message();
        return false;
    }
    error.clear();
    return true;
}

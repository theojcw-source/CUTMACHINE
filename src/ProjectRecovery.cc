#include "ProjectRecovery.h"

#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;

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
        error = "unable to create recovery temporary file '" + path.string() +
                "'";
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        error = "unable to write recovery temporary file '" + path.string() +
                "'";
        return false;
    }
    output.close();
    if (!output) {
        error = "unable to close recovery temporary file '" + path.string() +
                "'";
        return false;
    }
    return true;
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

    const fs::path autosave(AutosavePath(projectPath));
    const fs::path temporaryPath =
        fs::path(autosave.string() + "." + GenerateUlid() + ".tmp");
    TemporaryFile temporary(temporaryPath);
    if (!WriteFile(temporary.path(), document.SaveToString(), error)) {
        return false;
    }

    std::error_code renameError;
    fs::rename(temporary.path(), autosave, renameError);
    if (renameError) {
        error = "unable to atomically replace autosave '" + autosave.string() +
                "': " + renameError.message();
        return false;
    }
    temporary.Commit();
    error.clear();
    return true;
}

ProjectRecoveryInfo ProjectRecovery::Inspect(
    const std::string& projectPath) {
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
        info.error = "autosave is not a regular file: '" +
                     info.autosave_path + "'";
        return info;
    }

    Document recovered;
    if (!Document::Load(info.autosave_path, recovered, info.error)) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "invalid autosave '" + info.autosave_path + "': " +
                     info.error;
        return info;
    }

    statusError.clear();
    const fs::file_status projectStatus = fs::status(projectPath, statusError);
    if (statusError != std::errc::no_such_file_or_directory && statusError) {
        info.state = ProjectRecoveryState::Invalid;
        info.error = "unable to inspect project '" + projectPath + "': " +
                     statusError.message();
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
        error = "refusing to discard autosave directory '" +
                autosave.string() + "'";
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

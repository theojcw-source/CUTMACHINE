#pragma once

#include "Document.h"
#include "EditLog.h"

#include <map>
#include <string>

class Project;

// Autosaves are deliberately derived state. The project remains authoritative
// until a caller explicitly promotes a recovered Document or Project.
enum class ProjectRecoveryState {
    None,
    Stale,
    Available,
    Invalid,
};

enum class ProjectRecoveryFormat {
    Unknown,
    Document,
    Project,
    Envelope,
};

struct ProjectRecoveryInfo {
    ProjectRecoveryState state = ProjectRecoveryState::None;
    ProjectRecoveryFormat format = ProjectRecoveryFormat::Unknown;
    std::string project_path;
    std::string autosave_path;
    std::string error;
};

class ProjectRecovery {
public:
    // The deterministic sidecar name makes the derived nature of the file
    // visible and lets every surface address the same recovery candidate.
    static std::string AutosavePath(const std::string& projectPath);

    // Writes canonical document JSON to a sibling temporary file, then
    // atomically replaces the autosave. Never writes the project itself.
    static bool WriteAutosave(const std::string& projectPath,
                              const Document& document, std::string& error);
    static bool WriteAutosave(const std::string& projectPath,
                              const Project& project, std::string& error);
    // The versioned envelope preserves the project and both history levels as
    // one atomic recovery candidate. Every project timeline must have a log.
    static bool WriteAutosave(
        const std::string& projectPath, const Project& project,
        const std::map<std::string, EditLog>& timelineLogs,
        const ProjectEditLog& projectLog, std::string& error);

    // Validates either canonical Document or Project JSON before reporting the
    // autosave as recoverable, and identifies its format for the caller. An
    // autosave is Available only when it is newer than the project, or the
    // project does not exist. Equal/older candidates are Stale.
    static ProjectRecoveryInfo Inspect(const std::string& projectPath);

    // Loads only the derived autosave. Output is unchanged on failure.
    static bool LoadAutosave(const std::string& projectPath, Document& output,
                             std::string& error);
    static bool LoadAutosave(const std::string& projectPath, Project& output,
                             std::string& error);
    // Loads a versioned envelope. Legacy Project autosaves remain accepted and
    // receive empty logs, so callers can migrate without discarding recovery.
    // Every output is unchanged on failure.
    static bool LoadAutosave(const std::string& projectPath,
                             Project& projectOutput,
                             std::map<std::string, EditLog>& timelineLogsOutput,
                             ProjectEditLog& projectLogOutput,
                             std::string& error);

    // Removes only the deterministic autosave sidecar. Missing files are a
    // successful no-op; directories and other removal failures are reported.
    static bool DiscardAutosave(const std::string& projectPath,
                                std::string& error);
};

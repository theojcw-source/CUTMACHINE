#pragma once

#include "Document.h"

#include <string>

class Project;

// Autosaves are deliberately derived state. The project JSON remains the
// authority until a caller explicitly saves a recovered Document to it.
enum class ProjectRecoveryState {
    None,
    Stale,
    Available,
    Invalid,
};

struct ProjectRecoveryInfo {
    ProjectRecoveryState state = ProjectRecoveryState::None;
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

    // Validates the autosave before reporting it as recoverable. An autosave
    // is Available only when it is newer than the project, or the project does
    // not exist. Equal/older candidates are Stale.
    static ProjectRecoveryInfo Inspect(const std::string& projectPath);

    // Loads only the derived autosave. Output is unchanged on failure.
    static bool LoadAutosave(const std::string& projectPath, Document& output,
                             std::string& error);
    static bool LoadAutosave(const std::string& projectPath, Project& output,
                             std::string& error);

    // Removes only the deterministic autosave sidecar. Missing files are a
    // successful no-op; directories and other removal failures are reported.
    static bool DiscardAutosave(const std::string& projectPath,
                                std::string& error);
};

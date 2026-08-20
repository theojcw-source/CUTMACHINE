#include "Document.h"
#include "EditLog.h"
#include "Project.h"
#include "ProjectRecovery.h"
#include "Ulid.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(fs::temp_directory_path() /
                ("cutmachine-recovery-" + GenerateUlid())) {
        fs::create_directory(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

Document ValidDocument(const std::string& name) {
    Document document;
    document.sequence.name = name;
    return document;
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    if (!output) throw std::runtime_error("test fixture write failed");
}

}  // namespace

int main() {
    Test("autosave canonical round trip leaves project untouched", [] {
        TemporaryDirectory directory;
        const fs::path project = directory.path() / "edit.cut.json";
        const Document saved = ValidDocument("Saved truth");
        const Document working = ValidDocument("Unsaved recovery");
        std::string error;
        Check(saved.Save(project.string(), error), "save project: " + error);
        const std::string projectBefore = Read(project);

        Check(ProjectRecovery::WriteAutosave(project.string(), working, error),
              "write autosave: " + error);
        Check(Read(project) == projectBefore,
              "autosave must not mutate the authoritative project");
        const fs::path autosave =
            ProjectRecovery::AutosavePath(project.string());
        Check(Read(autosave) == working.SaveToString(),
              "autosave must contain canonical document JSON");

        Document recovered = ValidDocument("Sentinel");
        Check(ProjectRecovery::LoadAutosave(project.string(), recovered, error),
              "load autosave: " + error);
        Check(recovered.SaveToString() == working.SaveToString(),
              "loaded recovery must round-trip byte-identically");
        Check(Read(project) == projectBefore,
              "loading recovery must not promote it implicitly");
    });

    Test("corrupt autosave is invalid and load preserves output", [] {
        TemporaryDirectory directory;
        const fs::path project = directory.path() / "edit.json";
        std::string error;
        Check(ValidDocument("Saved").Save(project.string(), error),
              "save project: " + error);
        Write(ProjectRecovery::AutosavePath(project.string()), "{broken");

        const ProjectRecoveryInfo info =
            ProjectRecovery::Inspect(project.string());
        Check(info.state == ProjectRecoveryState::Invalid,
              "corrupt autosave must inspect as invalid");
        Check(info.format == ProjectRecoveryFormat::Unknown,
              "corrupt autosave must not claim a recovery format");
        Check(!info.error.empty(), "invalid candidate must explain why");

        Document output = ValidDocument("Unchanged sentinel");
        const std::string before = output.SaveToString();
        Check(!ProjectRecovery::LoadAutosave(project.string(), output, error),
              "corrupt autosave must fail to load");
        Check(output.SaveToString() == before,
              "failed load must leave output unchanged");

        Project projectOutput("Unchanged project sentinel");
        const std::string projectBefore = projectOutput.SaveToString();
        Check(!ProjectRecovery::LoadAutosave(project.string(), projectOutput,
                                             error),
              "corrupt project autosave must fail to load");
        Check(projectOutput.SaveToString() == projectBefore,
              "failed project load must leave output unchanged");
    });

    Test("autosave preserves the complete multi-timeline project", [] {
        TemporaryDirectory directory;
        const fs::path path = directory.path() / "multi.cut.json";
        Project saved("Saved project");
        ProjectEditLog projectLog;
        EditError editError = EditError::None;
        std::string error;
        Check(projectLog.Apply(saved,
                               ProjectOperation{AddProjectTimelineOperation{
                                   "Alternate", 1080, 1920, {25, 1}}},
                               editError, error),
              "add alternate timeline: " + error);
        Check(saved.Save(path.string(), error), "save project: " + error);
        Project working = saved;
        working.name = "Recovered project";
        working.active_timeline_id = working.timelines.back().id;
        Check(ProjectRecovery::WriteAutosave(path.string(), working, error),
              "write project autosave: " + error);

        const fs::path autosave = ProjectRecovery::AutosavePath(path.string());
        const auto base = fs::file_time_type::clock::now();
        fs::last_write_time(path, base);
        fs::last_write_time(autosave, base + std::chrono::seconds(2));
        const ProjectRecoveryInfo info =
            ProjectRecovery::Inspect(path.string());
        Check(info.state == ProjectRecoveryState::Available,
              "new project autosave must inspect as available");
        Check(info.format == ProjectRecoveryFormat::Project,
              "inspection must identify project autosave format");

        Project recovered("Sentinel");
        Check(ProjectRecovery::LoadAutosave(path.string(), recovered, error),
              "load project autosave: " + error);
        Check(recovered.SaveToString() == working.SaveToString(),
              "project recovery retains every timeline and active identity");
    });

    Test("inspection distinguishes stale and newer autosaves", [] {
        TemporaryDirectory directory;
        const fs::path project = directory.path() / "edit.json";
        const fs::path autosave =
            ProjectRecovery::AutosavePath(project.string());
        std::string error;
        Check(ValidDocument("Saved").Save(project.string(), error),
              "save project: " + error);
        Check(ProjectRecovery::WriteAutosave(project.string(),
                                             ValidDocument("Working"), error),
              "write autosave: " + error);

        const auto base = fs::file_time_type::clock::now();
        fs::last_write_time(autosave, base - std::chrono::seconds(2));
        fs::last_write_time(project, base);
        Check(ProjectRecovery::Inspect(project.string()).state ==
                  ProjectRecoveryState::Stale,
              "older autosave must be stale");
        Check(ProjectRecovery::Inspect(project.string()).format ==
                  ProjectRecoveryFormat::Document,
              "inspection must identify document autosave format");

        fs::last_write_time(autosave, base + std::chrono::seconds(2));
        Check(ProjectRecovery::Inspect(project.string()).state ==
                  ProjectRecoveryState::Available,
              "newer autosave must be recoverable");

        fs::remove(project);
        Check(ProjectRecovery::Inspect(project.string()).state ==
                  ProjectRecoveryState::Available,
              "valid autosave without a project must be recoverable");
    });

    Test("inspection classifies project autosave freshness", [] {
        TemporaryDirectory directory;
        const fs::path projectPath = directory.path() / "multi.cut.json";
        const fs::path autosave =
            ProjectRecovery::AutosavePath(projectPath.string());
        Project saved("Saved project");
        Project working = saved;
        working.name = "Working project";
        std::string error;
        Check(saved.Save(projectPath.string(), error),
              "save project: " + error);
        Check(ProjectRecovery::WriteAutosave(projectPath.string(), working,
                                             error),
              "write project autosave: " + error);

        const auto base = fs::file_time_type::clock::now();
        fs::last_write_time(autosave, base - std::chrono::seconds(2));
        fs::last_write_time(projectPath, base);
        ProjectRecoveryInfo info =
            ProjectRecovery::Inspect(projectPath.string());
        Check(info.state == ProjectRecoveryState::Stale,
              "older project autosave must be stale");
        Check(info.format == ProjectRecoveryFormat::Project,
              "stale project autosave must retain its detected format");

        fs::last_write_time(autosave, base + std::chrono::seconds(2));
        info = ProjectRecovery::Inspect(projectPath.string());
        Check(info.state == ProjectRecoveryState::Available,
              "newer project autosave must be recoverable");
        Check(info.format == ProjectRecoveryFormat::Project,
              "available project autosave must retain its detected format");

        fs::remove(projectPath);
        info = ProjectRecovery::Inspect(projectPath.string());
        Check(info.state == ProjectRecoveryState::Available,
              "project autosave without a project must be recoverable");
        Check(info.format == ProjectRecoveryFormat::Project,
              "orphaned project autosave must retain its detected format");

        Write(autosave, "{broken");
        info = ProjectRecovery::Inspect(projectPath.string());
        Check(info.state == ProjectRecoveryState::Invalid,
              "corrupt project candidate must inspect as invalid");
        Check(info.format == ProjectRecoveryFormat::Unknown,
              "invalid project candidate must not claim a format");
    });

    Test("versioned envelope round trips project and complete history", [] {
        TemporaryDirectory directory;
        const fs::path path = directory.path() / "history.cut.json";
        Project project("Recovered with history");
        ProjectEditLog projectLog;
        EditError editError = EditError::None;
        std::string error;
        Check(projectLog.Apply(project,
                               ProjectOperation{AddProjectTimelineOperation{
                                   "Alternate", 1080, 1920, {25, 1}}},
                               editError, error),
              "add timeline through project log: " + error);

        std::map<std::string, EditLog> timelineLogs;
        for (const DocumentSequence& timeline : project.timelines)
            timelineLogs.emplace(timeline.id, EditLog{});
        Document active = project.MakeActiveDocument();
        DocumentMarker marker;
        marker.name = "Recovery marker";
        marker.time = {12, 25};
        EditLog& activeLog = timelineLogs.at(project.active_timeline_id);
        Check(activeLog.Apply(active, AddMarkerOperation{marker}, editError,
                              error),
              "add marker through timeline log: " + error);
        Check(project.CommitActiveDocument(active, error),
              "commit operated timeline snapshot: " + error);

        Check(ProjectRecovery::WriteAutosave(path.string(), project,
                                             timelineLogs, projectLog, error),
              "write recovery envelope: " + error);
        const fs::path autosave = ProjectRecovery::AutosavePath(path.string());
        const std::string firstBytes = Read(autosave);
        Check(ProjectRecovery::WriteAutosave(path.string(), project,
                                             timelineLogs, projectLog, error),
              "rewrite recovery envelope: " + error);
        Check(Read(autosave) == firstBytes,
              "equivalent recovery envelopes must be byte-identical");

        const ProjectRecoveryInfo info =
            ProjectRecovery::Inspect(path.string());
        Check(info.state == ProjectRecoveryState::Available,
              "orphaned recovery envelope must be available");
        Check(info.format == ProjectRecoveryFormat::Envelope,
              "inspection must identify the versioned envelope");

        Project recovered("Sentinel");
        std::map<std::string, EditLog> recoveredLogs;
        ProjectEditLog recoveredProjectLog;
        Check(ProjectRecovery::LoadAutosave(path.string(), recovered,
                                            recoveredLogs, recoveredProjectLog,
                                            error),
              "load recovery envelope: " + error);
        Check(recovered.SaveToString() == project.SaveToString(),
              "envelope must preserve canonical project bytes");
        Check(recoveredLogs.size() == timelineLogs.size(),
              "envelope must preserve every timeline log");
        for (const auto& entry : timelineLogs) {
            Check(recoveredLogs.count(entry.first) == 1,
                  "recovered log map must preserve timeline identity");
            if (recoveredLogs.count(entry.first) == 1) {
                Check(recoveredLogs.at(entry.first).Serialize() ==
                          entry.second.Serialize(),
                      "timeline log must round-trip byte-identically");
            }
        }
        Check(recoveredProjectLog.Serialize() == projectLog.Serialize(),
              "project log must round-trip byte-identically");

        Project projectOnly("Project-only sentinel");
        Check(ProjectRecovery::LoadAutosave(path.string(), projectOnly, error),
              "legacy project-only overload must read an envelope: " + error);
        Check(projectOnly.SaveToString() == project.SaveToString(),
              "project-only overload must expose the enveloped project");
    });

    Test("envelope rejects incomplete history before replacement", [] {
        TemporaryDirectory directory;
        const fs::path path = directory.path() / "history.cut.json";
        Project project("Complete history");
        std::map<std::string, EditLog> completeLogs;
        completeLogs.emplace(project.active_timeline_id, EditLog{});
        ProjectEditLog projectLog;
        std::string error;
        Check(ProjectRecovery::WriteAutosave(path.string(), project,
                                             completeLogs, projectLog, error),
              "write initial envelope: " + error);
        const fs::path autosave = ProjectRecovery::AutosavePath(path.string());
        const std::string before = Read(autosave);

        std::map<std::string, EditLog> missingLogs;
        Check(!ProjectRecovery::WriteAutosave(path.string(), project,
                                              missingLogs, projectLog, error),
              "missing timeline history must be rejected");
        Check(Read(autosave) == before,
              "rejected envelope must preserve the prior autosave");
        Check(error.find("exactly one edit log") != std::string::npos,
              "incomplete history error must explain the invariant");
    });

    Test("corrupt envelope preserves every recovery output", [] {
        TemporaryDirectory directory;
        const fs::path path = directory.path() / "history.cut.json";
        Project project("Corrupted history");
        std::map<std::string, EditLog> logs;
        logs.emplace(project.active_timeline_id, EditLog{});
        ProjectEditLog projectLog;
        std::string error;
        Check(ProjectRecovery::WriteAutosave(path.string(), project, logs,
                                             projectLog, error),
              "write envelope: " + error);
        const fs::path autosave = ProjectRecovery::AutosavePath(path.string());
        std::string corrupt = Read(autosave);
        const std::string version = "\\\"version\\\":1";
        const size_t versionPosition = corrupt.find(version);
        Check(versionPosition != std::string::npos,
              "fixture must locate an embedded log version");
        if (versionPosition != std::string::npos)
            corrupt[versionPosition + version.size() - 1] = '9';
        Write(autosave, corrupt);

        Check(ProjectRecovery::Inspect(path.string()).state ==
                  ProjectRecoveryState::Invalid,
              "corrupt embedded history must invalidate inspection");
        Project outputProject("Unchanged project");
        std::map<std::string, EditLog> outputLogs;
        outputLogs.emplace("sentinel", EditLog{});
        ProjectEditLog outputProjectLog;
        const std::string projectBefore = outputProject.SaveToString();
        const std::string logBefore = outputLogs.at("sentinel").Serialize();
        const std::string projectLogBefore = outputProjectLog.Serialize();
        Check(
            !ProjectRecovery::LoadAutosave(path.string(), outputProject,
                                           outputLogs, outputProjectLog, error),
            "corrupt envelope must fail to load");
        Check(outputProject.SaveToString() == projectBefore,
              "failed envelope load must preserve project output");
        Check(outputLogs.size() == 1 && outputLogs.count("sentinel") == 1 &&
                  outputLogs.at("sentinel").Serialize() == logBefore,
              "failed envelope load must preserve timeline log output");
        Check(outputProjectLog.Serialize() == projectLogBefore,
              "failed envelope load must preserve project log output");
    });

    Test("history overload accepts legacy project autosaves", [] {
        TemporaryDirectory directory;
        const fs::path path = directory.path() / "legacy.cut.json";
        Project legacy("Legacy project");
        std::string error;
        Check(ProjectRecovery::WriteAutosave(path.string(), legacy, error),
              "write legacy project autosave: " + error);

        Project recovered("Sentinel");
        std::map<std::string, EditLog> recoveredLogs;
        ProjectEditLog recoveredProjectLog;
        Check(ProjectRecovery::LoadAutosave(path.string(), recovered,
                                            recoveredLogs, recoveredProjectLog,
                                            error),
              "load legacy project through history overload: " + error);
        Check(recovered.SaveToString() == legacy.SaveToString(),
              "legacy project must remain readable");
        Check(recoveredLogs.size() == legacy.timelines.size() &&
                  recoveredLogs.count(legacy.active_timeline_id) == 1,
              "legacy project must receive empty per-timeline histories");
        Check(recoveredProjectLog.Serialize() == ProjectEditLog{}.Serialize(),
              "legacy project must receive an empty project history");
    });

    Test("discard is exact safe and idempotent", [] {
        TemporaryDirectory directory;
        const fs::path project = directory.path() / "edit.json";
        const fs::path autosave =
            ProjectRecovery::AutosavePath(project.string());
        std::string error;
        Check(ValidDocument("Saved").Save(project.string(), error),
              "save project: " + error);
        const std::string projectBefore = Read(project);
        Check(ProjectRecovery::WriteAutosave(project.string(),
                                             ValidDocument("Working"), error),
              "write autosave: " + error);
        Check(ProjectRecovery::DiscardAutosave(project.string(), error),
              "discard autosave: " + error);
        Check(!fs::exists(autosave), "discard must remove the autosave");
        Check(Read(project) == projectBefore,
              "discard must leave the project untouched");
        Check(ProjectRecovery::DiscardAutosave(project.string(), error),
              "discarding a missing autosave must be a safe no-op");

        fs::create_directory(autosave);
        Check(!ProjectRecovery::DiscardAutosave(project.string(), error),
              "discard must refuse an autosave directory");
        Check(fs::is_directory(autosave),
              "refused directory must remain untouched");
    });

    Test("failed autosave does not overwrite last valid recovery", [] {
        TemporaryDirectory directory;
        const fs::path project = directory.path() / "edit.json";
        const fs::path autosave =
            ProjectRecovery::AutosavePath(project.string());
        std::string error;
        const Document valid = ValidDocument("Last valid recovery");
        Check(ProjectRecovery::WriteAutosave(project.string(), valid, error),
              "write initial autosave: " + error);
        const std::string before = Read(autosave);

        Document invalid = ValidDocument("Invalid replacement");
        invalid.sequence.width = 0;
        Check(!ProjectRecovery::WriteAutosave(project.string(), invalid, error),
              "invalid document must fail before replacement");
        Check(Read(autosave) == before,
              "failed autosave must preserve last valid recovery bytes");
        Check(error.find("invalid document") != std::string::npos,
              "failed autosave must provide a useful error");

        size_t temporaryCount = 0;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory.path())) {
            if (entry.path().extension() == ".tmp") ++temporaryCount;
        }
        Check(temporaryCount == 0,
              "failed autosave must not leave temporary files");
    });

    Test("empty project path is rejected", [] {
        std::string error;
        Check(ProjectRecovery::AutosavePath("").empty(),
              "empty project has no derived autosave path");
        Check(!ProjectRecovery::WriteAutosave("", ValidDocument("Working"),
                                              error),
              "write must reject empty project path");
        Check(!ProjectRecovery::DiscardAutosave("", error),
              "discard must reject empty project path");
        Check(
            ProjectRecovery::Inspect("").state == ProjectRecoveryState::Invalid,
            "inspection must reject empty project path");
    });

    if (failures != 0) {
        std::cerr << failures << " project recovery test(s) failed\n";
        return 1;
    }
    std::cout << "All project recovery tests passed\n";
    return 0;
}

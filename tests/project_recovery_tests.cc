#include "Document.h"
#include "EditLog.h"
#include "Project.h"
#include "ProjectRecovery.h"
#include "Ulid.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        Check(!info.error.empty(), "invalid candidate must explain why");

        Document output = ValidDocument("Unchanged sentinel");
        const std::string before = output.SaveToString();
        Check(!ProjectRecovery::LoadAutosave(project.string(), output, error),
              "corrupt autosave must fail to load");
        Check(output.SaveToString() == before,
              "failed load must leave output unchanged");
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

        fs::last_write_time(autosave, base + std::chrono::seconds(2));
        Check(ProjectRecovery::Inspect(project.string()).state ==
                  ProjectRecoveryState::Available,
              "newer autosave must be recoverable");

        fs::remove(project);
        Check(ProjectRecovery::Inspect(project.string()).state ==
                  ProjectRecoveryState::Available,
              "valid autosave without a project must be recoverable");
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

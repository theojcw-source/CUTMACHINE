#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

Document Fixture() {
    Document document;
    document.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000002",
         "video",
         0,
         {{"01K30000000000000000000003",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}},
          {"01K30000000000000000000004",
           "01K30000000000000000000001",
           {200, 25},
           {10, 25},
           {20, 25}}}},
    };
    document.sequence.markers = {
        {"01K30000000000000000000005",
         "Premier raccord",
         {15, 25},
         "#33AAFF",
         "Montage"},
    };
    return document;
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-cli-tests");
    std::filesystem::create_directory(directory);
    std::string error;
    std::string createdOutput;
    const std::filesystem::path createdPackage =
        directory / "Created.cutmachine-project";
    Check(CreateProjectCommand(createdPackage.string(), "Mission IA",
                               createdOutput) == 0,
          "headless project creation succeeds: " + createdOutput);
    const std::filesystem::path createdPath =
        createdPackage / "project.cutmachine.json";
    Project createdProject;
    Check(Project::Load(createdPath.string(), createdProject, error) &&
              createdProject.name == "Mission IA" &&
              createdProject.timelines.size() == 1,
          "headless project creation writes a valid package: " + error);
    const std::string createdBytes = Read(createdPath);
    Check(CreateProjectCommand(createdPackage.string(), "Replacement",
                               createdOutput) == 1,
          "headless project creation refuses an existing destination");
    Check(Read(createdPath) == createdBytes,
          "refused project creation preserves the existing package");
    Check(TranscribeMediaCommand(createdPath.string(),
                                 "01K39999999999999999999999", "missing.bin",
                                 "fr", true, createdOutput) == 1 &&
              createdOutput.find("\"error\":\"UnknownMedia\"") !=
                  std::string::npos,
          "headless transcription rejects an unknown media identity");
    Check(Read(createdPath) == createdBytes,
          "refused transcription leaves the project byte-identical");

    Project fixtureProject = Project::FromDocument(Fixture(), "CLI fixture");
    std::string fixturePath;
    Check(CreatePortableProject(
              (directory / "Fixture.cutmachine-project").string(),
              fixtureProject, fixturePath, error),
          "fixture package saves: " + error);
    const std::filesystem::path path = fixturePath;

    std::string firstDescription;
    std::string secondDescription;
    Check(DescribeCommand(path.string(), firstDescription) == 0,
          "describe succeeds");
    Check(DescribeCommand(path.string(), secondDescription) == 0,
          "second describe succeeds");
    Check(firstDescription == secondDescription, "describe is byte-stable");
    Check(firstDescription.find("\"alias\":\"A1\"") != std::string::npos &&
              firstDescription.find("\"alias\":\"A2\"") != std::string::npos,
          "describe emits stable per-track aliases");
    Check(firstDescription.find("\"type\":\"gap\"") != std::string::npos,
          "describe emits holes");
    Check(firstDescription.find("\"frames\":") != std::string::npos &&
              firstDescription.find("\"seconds\":") != std::string::npos,
          "describe emits frames and decimal seconds");
    Check(firstDescription.find("\"markers\":[{\"alias\":\"K1\"") !=
                  std::string::npos &&
              firstDescription.find("\"name\":\"Premier raccord\"") !=
                  std::string::npos,
          "describe exposes stable marker IDs and aliases");

    const std::string before = Read(path);
    const Operation trim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    std::string result;
    Check(ApplyOperationCommand(path.string(), SerializeOperation(trim),
                                result) == 0,
          "valid apply-op succeeds: " + result);
    Check(result.find("{\"ok\":true,\"doc_hash\":\"") == 0,
          "valid apply-op returns a document hash");
    const std::string after = Read(path);
    Check(after != before, "valid apply-op changes the document bytes");

    EditLog log;
    EditError editError = EditError::None;
    std::string detail;
    Project appliedProject;
    Check(Project::Load(path.string(), appliedProject, detail),
          "applied project reloads: " + detail);
    const std::string activeTimelineLog = TimelineEditLogPathForProject(
        path.string(), appliedProject.active_timeline_id);
    Check(EditLog::Load(activeTimelineLog, log, editError, detail),
          "sidecar edit log loads: " + detail);
    Check(log.AppliedCount() == 1, "valid apply-op increments edit log");

    const Operation addBin =
        AddBinOperation{"01K30000000000000000000009", "Rushes CLI"};
    Check(ApplyOperationCommand(path.string(), SerializeOperation(addBin),
                                result) == 0,
          "CLI creates a persistent bin through the same operation path");
    std::string withBinDescription;
    Check(DescribeCommand(path.string(), withBinDescription) == 0 &&
              withBinDescription.find("\"name\":\"Rushes CLI\"") !=
                  std::string::npos,
          "describe exposes bins created by apply-op");
    const std::string afterBin = Read(path);

    const std::string logBeforeRejection = Read(activeTimelineLog);
    const Operation rejected =
        RemoveClipOperation{"01K39999999999999999999999", {}};
    Check(ApplyOperationCommand(path.string(), SerializeOperation(rejected),
                                result) == 1,
          "refused apply-op returns status 1");
    Check(result.find("\"error\":\"UnknownClip\"") != std::string::npos,
          "refused apply-op returns the exact operation error name");
    Check(Read(path) == afterBin,
          "refused apply-op leaves document byte-identical");
    Check(Read(activeTimelineLog) == logBeforeRejection,
          "refused apply-op leaves edit log byte-identical");

    Check(ApplyOperationCommand(path.string(), "{not json", result) == 1,
          "malformed operation returns status 1");
    Check(result.find("\"error\":\"ParseError\"") != std::string::npos,
          "malformed operation returns ParseError");
    Check(Read(path) == afterBin,
          "malformed operation leaves document byte-identical");

    Project project = Project::FromDocument(Fixture(), "CLI project");
    std::string projectPathString;
    Check(CreatePortableProject(
              (directory / "Project.cutmachine-project").string(), project,
              projectPathString, error),
          "project fixture saves: " + error);
    const std::filesystem::path projectPath = projectPathString;
    const std::string projectBefore = Read(projectPath);
    const ProjectOperation addTimeline =
        AddProjectTimelineOperation{"CLI vertical",
                                    1080,
                                    1920,
                                    {25, 1},
                                    "01K30000000000000000000010",
                                    "01K30000000000000000000011",
                                    "01K30000000000000000000012"};
    Check(ApplyProjectOperationCommand(projectPath.string(),
                                       SerializeProjectOperation(addTimeline),
                                       result) == 0,
          "valid apply-project-op succeeds: " + result);
    Check(result.find("{\"ok\":true,\"project_hash\":\"") == 0,
          "apply-project-op returns a project hash");
    const std::string projectAfter = Read(projectPath);
    Check(projectAfter != projectBefore,
          "apply-project-op changes project bytes");

    ProjectEditLog projectLog;
    Check(
        ProjectEditLog::Load(ProjectEditLogPathForProject(projectPath.string()),
                             projectLog, editError, detail),
        "project edit log loads: " + detail);
    Check(projectLog.AppliedCount() == 1,
          "apply-project-op increments the project log");
    Check(UndoProjectOperationCommand(projectPath.string(), result) == 0,
          "undo-project-op succeeds: " + result);
    Check(Read(projectPath) == projectBefore,
          "CLI project undo restores byte-identical JSON");
    Check(RedoProjectOperationCommand(projectPath.string(), result) == 0,
          "redo-project-op succeeds: " + result);
    Check(Read(projectPath) == projectAfter,
          "CLI project redo restores deterministic JSON");

    const std::string projectLogBeforeRejection =
        Read(ProjectEditLogPathForProject(projectPath.string()));
    const ProjectOperation rejectedProject =
        RemoveProjectTimelineOperation{"01K39999999999999999999999"};
    Check(ApplyProjectOperationCommand(
              projectPath.string(), SerializeProjectOperation(rejectedProject),
              result) == 1,
          "refused project operation returns status 1");
    Check(result.find("\"error\":\"UnknownSequence\"") != std::string::npos,
          "refused project operation returns its exact error");
    Check(Read(projectPath) == projectAfter,
          "refused project operation leaves project byte-identical");
    Check(Read(ProjectEditLogPathForProject(projectPath.string())) ==
              projectLogBeforeRejection,
          "refused project operation leaves project log byte-identical");

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All CLI tests passed\n";
    return 0;
}

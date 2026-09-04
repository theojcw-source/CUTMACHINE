#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Ingest.h"
#include "Json.h"
#include "Operations.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "ResolveImport.h"
#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckFailureEnvelope(const std::string& output, const std::string& label,
                          const std::string& expectedError) {
    mcp_json::Value envelope;
    std::string parseError;
    const bool parsed =
        mcp_json::Value::Parse(output, envelope, parseError) &&
        envelope.IsObject();
    Check(parsed, label + " returns a JSON object: " + parseError);
    if (!parsed) return;
    const mcp_json::Value* ok = envelope.Find("ok");
    const mcp_json::Value* error = envelope.Find("error");
    const mcp_json::Value* detail = envelope.Find("detail");
    Check(ok != nullptr && ok->IsBool() && !ok->AsBool(),
          label + " returns ok:false");
    Check(error != nullptr && error->IsString() &&
              error->AsString() == expectedError,
          label + " preserves " + expectedError);
    Check(detail != nullptr && detail->IsString() &&
              !detail->AsString().empty(),
          label + " returns a non-empty detail");
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

    // B2 -- exercise every headless command at its cheapest refusal point.
    // This catalog catches a new command that returns a non-zero status with
    // free text, a success-shaped object, or a missing stable code.
    {
        const std::filesystem::path missingProject =
            directory / "absent.cutmachine.json";
        const std::filesystem::path missingManifest =
            directory / "absent-resolve.json";
        struct FailureCase {
            std::string name;
            std::string error;
            std::function<int(std::string&)> call;
        };
        const std::vector<FailureCase> cases = {
            {"create_project", "InvalidOperation",
             [&](std::string& output) {
                 return CreateProjectCommand(
                     (directory / "unused.cutmachine-project").string(), "",
                     output);
             }},
            {"transcribe_media", "ParseError",
             [&](std::string& output) {
                 return TranscribeMediaCommand(
                     missingProject.string(), {"missing-media"}, "", "auto",
                     false, false, output);
             }},
            {"analyze_shot_quality", "ParseError",
             [&](std::string& output) {
                 return AnalyzeShotQualityCommand(
                     missingProject.string(), "missing-media", output);
             }},
            {"shot_quality_report", "ParseError",
             [&](std::string& output) {
                 return ShotQualityReportCommand(missingProject.string(),
                                                 output);
             }},
            {"align_transcripts", "ParseError",
             [&](std::string& output) {
                 return AlignTranscriptsCommand(missingProject.string(), false,
                                                output);
             }},
            {"analyze_speech_onset", "ParseError",
             [&](std::string& output) {
                 return AnalyzeSpeechOnsetCommand(
                     missingProject.string(), "missing-media", output);
             }},
            {"speech_onset_report", "ParseError",
             [&](std::string& output) {
                 return SpeechOnsetReportCommand(missingProject.string(),
                                                 output);
             }},
            {"list_disfluencies", "ParseError",
             [&](std::string& output) {
                 return ListDisfluenciesCommand(missingProject.string(),
                                                "missing-clip", output);
             }},
            {"remove_words", "ParseError",
             [&](std::string& output) {
                 return RemoveWordsCommand(missingProject.string(),
                                           "missing-clip", "[]", output);
             }},
            {"tighten_pauses", "ParseError",
             [&](std::string& output) {
                 return TightenPausesCommand(missingProject.string(),
                                             "missing-clip", 400, 6, output);
             }},
            {"locate_source_frame", "ParseError",
             [&](std::string& output) {
                 return LocateSourceFrameCommand(
                     missingProject.string(), "missing-media", 0, output);
             }},
            {"describe", "ParseError",
             [&](std::string& output) {
                 return DescribeCommand(missingProject.string(), output);
             }},
            {"export_srt", "InvalidDocument",
             [&](std::string& output) {
                 return ExportSrtCommand(missingProject.string(),
                                         (directory / "unused.srt").string(),
                                         output);
             }},
            {"propose_sequence", "ParseError",
             [&](std::string& output) {
                 return ProposeSequenceCommand(missingProject.string(),
                                               output);
             }},
            {"apply_operation", "ParseError",
             [&](std::string& output) {
                 return ApplyOperationCommand(missingProject.string(), "{}",
                                              output);
             }},
            {"apply_project_operation", "ParseError",
             [&](std::string& output) {
                 return ApplyProjectOperationCommand(missingProject.string(),
                                                     "{}", output);
             }},
            {"undo_project_operation", "ParseError",
             [&](std::string& output) {
                 return UndoProjectOperationCommand(missingProject.string(),
                                                    output);
             }},
            {"redo_project_operation", "ParseError",
             [&](std::string& output) {
                 return RedoProjectOperationCommand(missingProject.string(),
                                                    output);
             }},
            {"undo_operation", "ParseError",
             [&](std::string& output) {
                 return UndoOperationCommand(missingProject.string(), output);
             }},
            {"redo_operation", "ParseError",
             [&](std::string& output) {
                 return RedoOperationCommand(missingProject.string(), output);
             }},
            {"export", "InvalidDocument",
             [&](std::string& output) {
                 return ExportCommand(missingProject.string(), ExportSettings{},
                                      {}, nullptr, output);
             }},
            {"ingest", "ParseError",
             [&](std::string& output) {
                 return IngestCommand(missingProject.string(),
                                      directory.string(), false, output);
             }},
            {"export_resolve_timeline", "InvalidDocument",
             [&](std::string& output) {
                 return ExportResolveTimelineCommand(missingProject.string(),
                                                     output);
             }},
            {"import_resolve", "IoError",
             [&](std::string& output) {
                 return ImportResolveCommand(missingProject.string(),
                                             missingManifest.string(), output);
             }},
        };
        for (const FailureCase& test : cases) {
            std::string output;
            Check(test.call(output) != 0, test.name + " is refused");
            CheckFailureEnvelope(output, test.name, test.error);
        }
    }

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
                                 {"01K39999999999999999999999"}, "missing.bin",
                                 "fr", true, false, createdOutput) == 1 &&
              createdOutput.find("\"error\":\"UnknownMedia\"") !=
                  std::string::npos,
          "headless transcription rejects an unknown media identity");
    // QC-2026-09 A3 -- a batch is refused whole when one of its members is
    // not a media of this project: silently transcribing the rest would
    // leave a caller believing it had all of them.
    Check(TranscribeMediaCommand(createdPath.string(), {}, "missing.bin", "fr",
                                 true, false, createdOutput) == 1 &&
              createdOutput.find("at least one media_id") != std::string::npos,
          "headless transcription refuses an empty batch with a reason");
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

    // QC-2026-09 A5 -- --apply-op is the CLI surface for synchronized ripple
    // removal; the undo command must restore every named track and the full
    // removed clip representation, not just its five timeline fields.
    Document syncFixture = Fixture();
    syncFixture.sequence.tracks.push_back(
        {"01K30000000000000000000020",
         "audio",
         1,
         {{"01K30000000000000000000021",
           syncFixture.sources[0].id,
           {400, 25},
           {10, 25},
           {20, 25}}}});
    Project syncProject = Project::FromDocument(syncFixture, "CLI sync");
    std::string syncPathString;
    Check(CreatePortableProject(
              (directory / "Sync.cutmachine-project").string(), syncProject,
              syncPathString, error),
          "synchronized CLI fixture saves: " + error);
    const std::filesystem::path syncPath = syncPathString;
    const std::string syncBefore = Read(syncPath);
    const Operation synchronizedRemove = RemoveClipOperation{
        "01K30000000000000000000003",
        {"01K30000000000000000000020"},
        {}};
    Check(ApplyOperationCommand(syncPath.string(),
                                SerializeOperation(synchronizedRemove),
                                result) == 0,
          "CLI synchronized remove applies: " + result);
    Project syncAfterProject;
    Check(Project::Load(syncPath.string(), syncAfterProject, error),
          "CLI synchronized result reloads: " + error);
    const Document syncAfter = syncAfterProject.MakeActiveDocument();
    Check(syncAfter.FindClip("01K30000000000000000000004")
                      ->timeline_in == RationalTime{10, 25} &&
              syncAfter.FindClip("01K30000000000000000000021")
                      ->timeline_in == RationalTime{10, 25},
          "CLI synchronized remove ripples the named track");
    Check(UndoOperationCommand(syncPath.string(), result) == 0 &&
              Read(syncPath) == syncBefore,
          "CLI synchronized undo restores canonical bytes");

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

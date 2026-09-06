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

#include <chrono>
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
    const bool parsed = mcp_json::Value::Parse(output, envelope, parseError) &&
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
    Check(
        detail != nullptr && detail->IsString() && !detail->AsString().empty(),
        label + " returns a non-empty detail");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string ProjectBatchJson(const std::vector<ProjectOperation>& operations) {
    std::string result = "[";
    for (size_t index = 0; index < operations.size(); ++index) {
        if (index) result += ',';
        result += SerializeProjectOperation(operations[index]);
    }
    return result + ']';
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
                 return TranscribeMediaCommand(missingProject.string(),
                                               {"missing-media"}, "", "auto",
                                               false, false, output);
             }},
            {"transcribe_timeline", "InvalidDocument",
             [&](std::string& output) {
                 return TranscribeTimelineCommand(missingProject.string(), "",
                                                  "auto", false, output);
             }},
            {"srt_from_media", "IoError",
             [&](std::string& output) {
                 return SrtFromMediaCommand(
                     (directory / "missing.wav").string(),
                     (directory / "unused.srt").string(), "auto", false,
                     output);
             }},
            {"analyze_shot_quality", "ParseError",
             [&](std::string& output) {
                 return AnalyzeShotQualityCommand(missingProject.string(),
                                                  "missing-media", output);
             }},
            {"shot_quality_report", "ParseError",
             [&](std::string& output) {
                 return ShotQualityReportCommand(missingProject.string(),
                                                 output);
             }},
            {"contact_sheet", "ParseError",
             [&](std::string& output) {
                 return ContactSheetCommand(
                     missingProject.string(),
                     (directory / "unused-contact.jpg").string(), "", 24,
                     output);
             }},
            {"cut_sheet", "ParseError",
             [&](std::string& output) {
                 return CutSheetCommand(
                     missingProject.string(),
                     (directory / "unused-cuts.jpg").string(), "", 24, output);
             }},
            {"align_transcripts", "ParseError",
             [&](std::string& output) {
                 return AlignTranscriptsCommand(missingProject.string(), false,
                                                output);
             }},
            {"analyze_speech_onset", "ParseError",
             [&](std::string& output) {
                 return AnalyzeSpeechOnsetCommand(missingProject.string(),
                                                  "missing-media", output);
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
            {"trim_boundary_air", "ParseError",
             [&](std::string& output) {
                 return TrimBoundaryAirCommand(missingProject.string(),
                                               "missing-clip", 3, 300, output);
             }},
            {"close_junction_air", "ParseError",
             [&](std::string& output) {
                 return CloseJunctionAirCommand(missingProject.string(),
                                                "missing-left", "missing-right",
                                                3, 300, output);
             }},
            {"locate_source_frame", "ParseError",
             [&](std::string& output) {
                 return LocateSourceFrameCommand(missingProject.string(),
                                                 "missing-media", 0, output);
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
                 return ProposeSequenceCommand(missingProject.string(), output);
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
    Check(
        firstDescription.find("\"timelines\":[{\"id\":") != std::string::npos &&
            firstDescription.find("\"width\":1920,\"height\":1080,") !=
                std::string::npos &&
            firstDescription.find("\"frame_rate\":{\"num\":25,") !=
                std::string::npos &&
            firstDescription.find("\"active\":true") != std::string::npos,
        "describe exposes the active project's timeline summary");

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
    syncFixture.sequence.tracks.push_back({"01K30000000000000000000020",
                                           "audio",
                                           1,
                                           {{"01K30000000000000000000021",
                                             syncFixture.sources[0].id,
                                             {400, 25},
                                             {10, 25},
                                             {20, 25}}}});
    Project syncProject = Project::FromDocument(syncFixture, "CLI sync");
    std::string syncPathString;
    Check(
        CreatePortableProject((directory / "Sync.cutmachine-project").string(),
                              syncProject, syncPathString, error),
        "synchronized CLI fixture saves: " + error);
    const std::filesystem::path syncPath = syncPathString;
    const std::string syncBefore = Read(syncPath);
    const Operation synchronizedRemove = RemoveClipOperation{
        "01K30000000000000000000003", {"01K30000000000000000000020"}, {}};
    Check(ApplyOperationCommand(syncPath.string(),
                                SerializeOperation(synchronizedRemove),
                                result) == 0,
          "CLI synchronized remove applies: " + result);
    Project syncAfterProject;
    Check(Project::Load(syncPath.string(), syncAfterProject, error),
          "CLI synchronized result reloads: " + error);
    const Document syncAfter = syncAfterProject.MakeActiveDocument();
    Check(syncAfter.FindClip("01K30000000000000000000004")->timeline_in ==
                  RationalTime{10, 25} &&
              syncAfter.FindClip("01K30000000000000000000021")->timeline_in ==
                  RationalTime{10, 25},
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

    // B10 -- ROADMAP.md, on the project journal. A timeline edit and a
    // project edit keep separate histories, and committing one must not
    // publish an empty version of the other: --apply-op and --undo used to
    // hand CommitStoredProjectAndLogs a default-constructed ProjectEditLog,
    // which erased every project undo step on each clip the agent moved.
    const std::string projectLogBeforeTimelineEdit =
        Read(ProjectEditLogPathForProject(projectPath.string()));
    const Operation timelineEditOnProject = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    Check(ApplyOperationCommand(projectPath.string(),
                                SerializeOperation(timelineEditOnProject),
                                result) == 0,
          "timeline edit on a project with history succeeds: " + result);
    Check(Read(ProjectEditLogPathForProject(projectPath.string())) ==
              projectLogBeforeTimelineEdit,
          "a timeline edit leaves the project journal byte-identical");
    Check(UndoOperationCommand(projectPath.string(), result) == 0,
          "undoing that timeline edit succeeds: " + result);
    Check(Read(ProjectEditLogPathForProject(projectPath.string())) ==
              projectLogBeforeTimelineEdit,
          "undoing a timeline edit leaves the project journal byte-identical");
    ProjectEditLog survivingProjectLog;
    Check(
        ProjectEditLog::Load(ProjectEditLogPathForProject(projectPath.string()),
                             survivingProjectLog, editError, detail) &&
            survivingProjectLog.AppliedCount() == 1,
        "project history survives timeline edits: " + detail);
    Check(UndoProjectOperationCommand(projectPath.string(), result) == 0,
          "project undo still reaches the step taken before the timeline "
          "edit: " +
              result);
    Check(RedoProjectOperationCommand(projectPath.string(), result) == 0,
          "project redo replays it: " + result);

    // B10 -- reproduce the measured project cardinality and the exact cleanup
    // shape. Corrupt journals are stronger than a timing-only assertion: an
    // eager reader would reject them, while the lazy project path must leave
    // every surviving byte untouched. Ten seconds is the product criterion,
    // not a microbenchmark threshold, and leaves ample room for loaded CI.
    Project largeProject("B10 performance fixture");
    for (int index = 1; index < 24; ++index) {
        DocumentSequence timeline;
        timeline.name = "Timeline " + std::to_string(index + 1);
        largeProject.timelines.push_back(std::move(timeline));
    }
    for (int index = 0; index < 210; ++index) {
        const Ulid mediaId = GenerateUlid();
        const std::string filename = "rush-" + std::to_string(index) + ".mov";
        LibraryMedia media;
        media.id = mediaId;
        media.path = filename;
        media.filename = filename;
        media.rate = {25, 1};
        media.duration = {250, 25};
        media.metadata_complete = false;
        largeProject.rushes.push_back(std::move(media));
        largeProject.sources.push_back({mediaId, filename, {25, 1}, {250, 25}});
    }
    std::string largeProjectPathString;
    Check(CreatePortableProject((directory / "B10.cutmachine-project").string(),
                                largeProject, largeProjectPathString, error),
          "B10 fixture saves: " + error);
    const std::filesystem::path largeProjectPath = largeProjectPathString;
    const std::string beforeLargeBatch = Read(largeProjectPath);

    std::map<Ulid, std::string> journalSentinels;
    for (const DocumentSequence& timeline : largeProject.timelines) {
        const std::string sentinel =
            "not a journal; must stay unread: " + timeline.id + "\n";
        journalSentinels.emplace(timeline.id, sentinel);
        Write(TimelineEditLogPathForProject(largeProjectPath.string(),
                                            timeline.id),
              sentinel);
    }
    std::vector<ProjectOperation> removals;
    for (size_t index = 9; index < largeProject.timelines.size(); ++index) {
        removals.push_back(
            RemoveProjectTimelineOperation{largeProject.timelines[index].id});
    }
    Check(removals.size() == 15,
          "B10 fixture contains exactly fifteen removals");
    const auto cleanupStart = std::chrono::steady_clock::now();
    Check(ApplyProjectOperationCommand(largeProjectPath.string(),
                                       ProjectBatchJson(removals), result) == 0,
          "fifteen project removals apply as one batch: " + result);
    const auto cleanupElapsed = std::chrono::steady_clock::now() - cleanupStart;
    Check(cleanupElapsed < std::chrono::seconds(10),
          "fifteen timeline removals stay below ten seconds");

    Project cleanedProject;
    Check(LoadStoredProject(largeProjectPath.string(), cleanedProject, error) &&
              cleanedProject.timelines.size() == 9,
          "B10 batch removes exactly fifteen timelines: " + error);
    ProjectEditLog batchProjectLog;
    Check(ProjectEditLog::Load(
              ProjectEditLogPathForProject(largeProjectPath.string()),
              batchProjectLog, editError, detail) &&
              batchProjectLog.AppliedCount() == 1,
          "B10 batch occupies one project undo entry: " + detail);
    for (const DocumentSequence& timeline : cleanedProject.timelines) {
        Check(Read(TimelineEditLogPathForProject(largeProjectPath.string(),
                                                 timeline.id)) ==
                  journalSentinels[timeline.id],
              "an untouched timeline journal remains byte-identical");
    }
    Check(UndoProjectOperationCommand(largeProjectPath.string(), result) == 0 &&
              Read(largeProjectPath) == beforeLargeBatch,
          "one project undo restores the entire fifteen-removal batch: " +
              result);
    Check(RedoProjectOperationCommand(largeProjectPath.string(), result) == 0,
          "one project redo reapplies the entire batch: " + result);

    const std::string beforeAtomicRefusal = Read(largeProjectPath);
    const std::string logBeforeAtomicRefusal =
        Read(ProjectEditLogPathForProject(largeProjectPath.string()));
    const Ulid survivingTimeline = cleanedProject.timelines.front().id;
    const std::vector<ProjectOperation> rejectedBatch = {
        RenameProjectItemOperation{survivingTimeline, "Must roll back"},
        RemoveProjectTimelineOperation{"01K39999999999999999999999"}};
    Check(ApplyProjectOperationCommand(largeProjectPath.string(),
                                       ProjectBatchJson(rejectedBatch),
                                       result) == 1,
          "a refused project batch returns status 1");
    CheckFailureEnvelope(result, "refused project batch", "UnknownSequence");
    Check(Read(largeProjectPath) == beforeAtomicRefusal &&
              Read(ProjectEditLogPathForProject(largeProjectPath.string())) ==
                  logBeforeAtomicRefusal,
          "a refused project batch is atomic on disk");

    Check(ApplyProjectOperationCommand(largeProjectPath.string(), "[]",
                                       result) == 1,
          "an empty project batch is refused");
    CheckFailureEnvelope(result, "empty project batch", "InvalidOperation");

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All CLI tests passed\n";
    return 0;
}

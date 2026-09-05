#include "EditLog.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    Project project("Commercial");
    Check(project.timelines.size() == 1, "a project starts with one timeline");
    Check(project.ActiveTimeline() == &project.timelines.front(),
          "the initial timeline is active");

    const Ulid firstId = project.active_timeline_id;
    const std::string beforeAdd = project.SaveToString();
    ProjectEditLog projectLog;
    EditError editError = EditError::None;
    std::string error;
    Check(projectLog.Apply(project,
                           ProjectOperation{AddProjectTimelineOperation{
                               "Social cut", 1080, 1920, {30000, 1001}}},
                           editError, error),
          "add timeline operation: " + error);
    const auto& added = std::get<AddProjectTimelineOperation>(
        projectLog.AppliedEntries().back().op);
    const Ulid verticalId = added.timeline_id;
    const std::string afterAdd = project.SaveToString();
    Check(project.FindTimeline(verticalId)->name == "Social cut" &&
              project.FindTimeline(verticalId)->width == 1080,
          "timeline-specific settings are retained");
    Check(projectLog.Undo(project, editError, error), "undo add: " + error);
    Check(project.SaveToString() == beforeAdd,
          "project undo restores byte-identical JSON");
    Check(projectLog.Redo(project, editError, error), "redo add: " + error);
    Check(project.SaveToString() == afterAdd &&
              project.FindTimeline(verticalId) != nullptr,
          "project redo retains generated timeline IDs");

    ProjectOperation decoded;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decoded, editError, error),
          "project operation round-trip: " + error);
    Check(std::get<AddProjectTimelineOperation>(decoded).timeline_id ==
              verticalId,
          "serialized project operation retains enriched IDs");

    // QC-2026-08 -- switching the active timeline is an operation like any
    // other, so an agent can navigate a project instead of being stuck on
    // whatever the app last left active.
    Check(project.active_timeline_id == firstId,
          "adding a timeline does not steal focus from the active one");
    const std::string beforeActivate = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{SetActiveProjectTimelineOperation{verticalId}},
              editError, error),
          "activate timeline operation: " + error);
    Check(project.active_timeline_id == verticalId &&
              project.ActiveTimeline() == project.FindTimeline(verticalId),
          "the named timeline becomes active");
    const std::string afterActivate = project.SaveToString();
    Check(projectLog.Undo(project, editError, error),
          "undo activate: " + error);
    Check(project.SaveToString() == beforeActivate &&
              project.active_timeline_id == firstId,
          "undoing an activation restores byte-identical JSON");
    Check(projectLog.Redo(project, editError, error),
          "redo activate: " + error);
    Check(project.SaveToString() == afterActivate,
          "redoing an activation is byte-identical too");
    ProjectOperation decodedActivate;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decodedActivate, editError, error) &&
              std::get<SetActiveProjectTimelineOperation>(decodedActivate)
                      .timeline_id == verticalId,
          "activation round-trips through canonical JSON: " + error);
    const std::string beforeUnknown = project.SaveToString();
    Check(!projectLog.Apply(project,
                            ProjectOperation{SetActiveProjectTimelineOperation{
                                "01K30000000000000000000099"}},
                            editError, error) &&
              editError == EditError::UnknownSequence,
          "activating a timeline the project does not hold is refused by name");
    Check(project.SaveToString() == beforeUnknown,
          "and the refusal leaves the project untouched");
    Check(projectLog.Apply(
              project,
              ProjectOperation{SetActiveProjectTimelineOperation{firstId}},
              editError, error),
          "restore the original active timeline: " + error);

    const std::string beforeSessionSelection = project.SaveToString();
    Document edit = project.MakeDocument(verticalId);
    Check(project.SaveToString() == beforeSessionSelection,
          "selecting a timeline snapshot is session state, not a mutation");

    edit.sequence.name = "Social cut v2";
    edit.color_management.enabled = true;
    Check(project.CommitDocument(verticalId, edit, error), "commit: " + error);
    Check(project.FindTimeline(verticalId)->name == "Social cut v2" &&
              project.settings.color_management.enabled,
          "the addressed edit snapshot commits into its project");

    const std::string beforeRemove = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{RemoveProjectTimelineOperation{verticalId}},
              editError, error),
          "remove timeline operation: " + error);
    ProjectOperation decodedRemove;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decodedRemove, editError, error),
          "remove operation round-trip: " + error);
    Check(project.active_timeline_id == firstId,
          "removing a non-default timeline preserves the project default");
    Check(projectLog.Undo(project, editError, error), "undo remove: " + error);
    Check(project.SaveToString() == beforeRemove,
          "undo remove restores the exact timeline");
    Check(projectLog.Redo(project, editError, error), "redo remove: " + error);
    Check(
        !projectLog.Apply(
            project, ProjectOperation{RemoveProjectTimelineOperation{firstId}},
            editError, error),
        "the final timeline cannot be removed");
    ProjectOperation invalidAdd =
        AddProjectTimelineOperation{"Invalid", 0, 1080, {25, 1}};
    const std::string invalidAddBefore = SerializeProjectOperation(invalidAdd);
    ProjectOperation ignoredInverse = AddProjectTimelineOperation{};
    Check(!ApplyProjectOperation(project, invalidAdd, ignoredInverse, editError,
                                 error),
          "invalid project operation is rejected");
    Check(SerializeProjectOperation(invalidAdd) == invalidAddBefore,
          "a rejected project operation is left byte-identical");
    Check(project.Validate(error), "valid project: " + error);

    Document wrongTimeline = project.MakeActiveDocument();
    wrongTimeline.sequence.id = GenerateUlid();
    const std::string unchangedName = project.ActiveTimeline()->name;
    Check(!project.CommitActiveDocument(wrongTimeline, error),
          "a snapshot from another timeline must be rejected");
    Check(project.ActiveTimeline()->name == unchangedName,
          "a rejected commit leaves the project unchanged");

    Project stored("Serialized project");
    stored.rushes = {{GenerateUlid(),
                      "rush.mov",
                      "rush.mov",
                      "h264",
                      true,
                      1920,
                      1080,
                      "yuv420p",
                      "tv",
                      "bt709",
                      "bt709",
                      "bt709",
                      0,
                      {25, 1},
                      {250, 25},
                      "landscape",
                      true,
                      48000,
                      2,
                      // audio_level_measured / audio_level (QC-2026-09 A3):
                      // this fixture is built by hand, never ingested.
                      false,
                      0,
                      "",
                      "",
                      true}};
    DocumentSource source;
    source.id = stored.rushes.front().id;
    source.path = "rush.mov";
    source.rate = {25, 1};
    source.duration = {250, 25};
    stored.sources = {source};
    stored.ActiveTimeline()->tracks = {
        {GenerateUlid(),
         "video",
         0,
         {{GenerateUlid(), source.id, {0, 25}, {25, 25}, {0, 25}}}}};
    ProjectEditLog storedLog;
    Check(storedLog.Apply(stored,
                          ProjectOperation{AddProjectTimelineOperation{
                              "Vertical", 1080, 1920, {25, 1}}},
                          editError, error),
          "add serialized timeline: " + error);
    const Ulid second = std::get<AddProjectTimelineOperation>(
                            storedLog.AppliedEntries().back().op)
                            .timeline_id;
    stored.timelines.back().tracks = {
        {GenerateUlid(),
         "video",
         0,
         {{GenerateUlid(), source.id, {25, 25}, {25, 25}, {0, 25}}}}};
    stored.active_timeline_id = second;
    stored.bin_metadata[source.id] = {"Hero shot", 5, {"select", "day"}, 7};
    Check(stored.Validate(error), "serializable project: " + error);

    Project shortProject = stored;
    ProjectEditLog shortLog;
    const std::string beforeShort = shortProject.SaveToString();
    CreateProjectTimelineFromSegmentsOperation shortOperation;
    shortOperation.name = "Short dynamique";
    shortOperation.width = stored.ActiveTimeline()->width;
    shortOperation.height = stored.ActiveTimeline()->height;
    shortOperation.frame_rate = stored.ActiveTimeline()->frame_rate;
    shortOperation.segments = {
        {source.id, {50, 25}, {25, 25}},
        {source.id, {0, 25}, {50, 25}},
    };
    Check(shortLog.Apply(shortProject, ProjectOperation{shortOperation},
                         editError, error),
          "create short timeline: " + error);
    const auto& appliedShort =
        std::get<CreateProjectTimelineFromSegmentsOperation>(
            shortLog.AppliedEntries().back().op);
    const std::string afterShort = shortProject.SaveToString();
    Check(shortProject.active_timeline_id == appliedShort.timeline_id &&
              shortProject.ActiveTimeline()->tracks.size() == 2 &&
              shortProject.ActiveTimeline()->tracks[0].clips.size() == 2 &&
              shortProject.ActiveTimeline()->tracks[1].clips.size() == 2 &&
              shortProject.ActiveTimeline()->tracks[0].clips[1].timeline_in ==
                  RationalTime{25, 25},
          "short assembly creates linked A/V clips in the requested order");
    ProjectOperation decodedShort;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(shortLog.AppliedEntries().back().op),
              decodedShort, editError, error),
          "short operation round-trip: " + error);
    Check(std::get<CreateProjectTimelineFromSegmentsOperation>(decodedShort)
                  .segments[0]
                  .video_clip_id == appliedShort.segments[0].video_clip_id,
          "short operation retains generated clip IDs");
    Check(shortLog.Undo(shortProject, editError, error),
          "undo short timeline: " + error);
    Check(shortProject.SaveToString() == beforeShort,
          "undo short timeline restores byte-identical project JSON");
    Check(shortLog.Redo(shortProject, editError, error),
          "redo short timeline: " + error);
    Check(shortProject.SaveToString() == afterShort,
          "redo short timeline restores byte-identical project JSON");

    // B10 -- one ordered project batch is one exact history step. The rename
    // deliberately depends on the preceding add, so reversing execution order
    // would fail instead of merely producing a different cosmetic result.
    Project batchProject("Batch project");
    const std::string beforeBatch = batchProject.SaveToString();
    const Ulid batchTimelineId = GenerateUlid();
    ProjectEditLog batchLog;
    Check(batchLog.ApplyBatch(
              batchProject,
              {ProjectOperation{AddProjectTimelineOperation{"Added first",
                                                            1920,
                                                            1080,
                                                            {25, 1},
                                                            batchTimelineId,
                                                            GenerateUlid(),
                                                            GenerateUlid()}},
               ProjectOperation{
                   RenameProjectItemOperation{batchTimelineId, "Renamed"}}},
              editError, error),
          "ordered project batch applies: " + error);
    const std::string afterBatch = batchProject.SaveToString();
    Check(batchProject.FindTimeline(batchTimelineId) != nullptr &&
              batchProject.FindTimeline(batchTimelineId)->name == "Renamed",
          "project batch applies operations in input order");
    Check(batchLog.AppliedCount() == 1,
          "project batch creates exactly one history entry");
    ProjectEditLog decodedBatchLog;
    Check(ProjectEditLog::Deserialize(batchLog.Serialize(), decodedBatchLog,
                                      editError, error) &&
              decodedBatchLog.AppliedCount() == 1,
          "project batch history round-trips canonically: " + error);
    Check(batchLog.Undo(batchProject, editError, error) &&
              batchProject.SaveToString() == beforeBatch,
          "one undo restores the bytes before a project batch: " + error);
    Check(batchLog.Redo(batchProject, editError, error) &&
              batchProject.SaveToString() == afterBatch,
          "one redo restores the bytes after a project batch: " + error);

    const std::string beforeRejectedBatch = batchProject.SaveToString();
    const std::string logBeforeRejectedBatch = batchLog.Serialize();
    Check(!batchLog.ApplyBatch(batchProject,
                               {ProjectOperation{RenameProjectItemOperation{
                                    batchTimelineId, "Must roll back"}},
                                ProjectOperation{RemoveProjectTimelineOperation{
                                    "01K39999999999999999999999"}}},
                               editError, error) &&
              editError == EditError::UnknownSequence,
          "a refusal makes the whole project batch fail");
    Check(batchProject.SaveToString() == beforeRejectedBatch &&
              batchLog.Serialize() == logBeforeRejectedBatch,
          "a refused project batch leaves project and history byte-identical");

    const std::string serialized = stored.SaveToString();
    Project loaded("placeholder");
    Check(Project::LoadFromString(serialized, loaded, error),
          "load serialized project: " + error);
    Check(loaded.SaveToString() == serialized,
          "project JSON round-trips byte-identically");
    Check(loaded.timelines.size() == 2 && loaded.active_timeline_id == second &&
              loaded.ActiveTimeline()->name == "Vertical",
          "all timelines and the active identity round-trip");
    Check(loaded.FindBinMetadata(source.id) &&
              loaded.FindBinMetadata(source.id)->rating == 5 &&
              loaded.FindBinMetadata(source.id)->tags.size() == 2,
          "project-bin metadata round-trips");

    Project noPromotion("placeholder");
    Check(!Project::LoadFromString(stored.MakeActiveDocument().SaveToString(),
                                   noPromotion, error),
          "standalone timeline documents are not promoted to projects");

    Project unchanged("unchanged");
    const Ulid unchangedId = unchanged.id;
    Check(!Project::LoadFromString("{\"project_format\":\"cutmachine-project\","
                                   "\"project_version\":99}",
                                   unchanged, error),
          "unsupported project versions are rejected");
    Check(unchanged.id == unchangedId,
          "failed project parsing leaves output unchanged");
    std::cout << "project tests passed\n";
    return 0;
}

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

    Document legacy = stored.MakeActiveDocument();
    const std::string legacyJson = legacy.SaveToString();
    Project promotedA("placeholder");
    Project promotedB("placeholder");
    Check(Project::LoadFromString(legacyJson, promotedA, error),
          "legacy document promotes to project: " + error);
    Check(Project::LoadFromString(legacyJson, promotedB, error),
          "legacy promotion repeats: " + error);
    Check(promotedA.timelines.size() == 1 &&
              promotedA.active_timeline_id == legacy.sequence.id,
          "legacy promotion preserves the sequence");
    Check(promotedA.id == promotedB.id,
          "legacy project identity migration is deterministic");

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

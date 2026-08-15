#include "EditLog.h"
#include "ProjectBin.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

LibraryMedia Rush(const Ulid& id, const std::string& name, const Ulid& binId) {
    LibraryMedia rush;
    rush.id = id;
    rush.path = name;
    rush.filename = name;
    rush.rate = {25, 1};
    rush.duration = {250, 25};
    rush.bin_id = binId;
    rush.metadata_complete = false;
    return rush;
}

DocumentSource Source(const Ulid& id, const std::string& name) {
    return {id, name, {25, 1}, {250, 25}};
}
}  // namespace

int main() {
    Project project("Documentary");
    const Ulid selects = GenerateUlid();
    const Ulid interviews = GenerateUlid();
    project.bins = {{selects, "Selects", ""},
                    {interviews, "Interviews", selects}};

    const Ulid clip10 = GenerateUlid();
    const Ulid clip2 = GenerateUlid();
    project.rushes = {Rush(clip10, "Clip 10.mov", interviews),
                      Rush(clip2, "Clip 2.mov", interviews)};
    project.sources = {Source(clip10, "Clip 10.mov"),
                       Source(clip2, "Clip 2.mov")};
    project.ActiveTimeline()->tracks = {
        {GenerateUlid(),
         "video",
         0,
         {{GenerateUlid(), clip10, {0, 25}, {50, 25}, {0, 25}}}}};

    std::string error;
    EditError editError = EditError::None;
    ProjectEditLog projectLog;
    const std::string beforeMetadata = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{SetProjectBinMetadataOperation{
                  clip10, {"Hero interview", 5, {"red", "dialogue"}, 2}}},
              editError, error),
          "metadata: " + error);
    const std::string afterMetadata = project.SaveToString();
    ProjectOperation decodedMetadata;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decodedMetadata, editError, error),
          "metadata operation round-trip: " + error);
    Check(projectLog.Undo(project, editError, error),
          "undo metadata: " + error);
    Check(project.SaveToString() == beforeMetadata,
          "metadata undo is byte-identical");
    Check(projectLog.Redo(project, editError, error),
          "redo metadata: " + error);
    Check(project.SaveToString() == afterMetadata,
          "metadata redo is deterministic");
    Check(projectLog.Apply(project,
                           ProjectOperation{SetProjectBinMetadataOperation{
                               clip2, {"B-roll", 3, {"blue"}, 1}}},
                           editError, error),
          "metadata: " + error);
    Check(projectLog.Apply(
              project, ProjectOperation{AddProjectTimelineOperation{"Trailer"}},
              editError, error),
          "add trailer: " + error);
    const Ulid trailer = std::get<AddProjectTimelineOperation>(
                             projectLog.AppliedEntries().back().op)
                             .timeline_id;
    const std::string beforePlacement = project.SaveToString();
    Check(projectLog.Apply(project,
                           ProjectOperation{SetProjectTimelineBinOperation{
                               trailer, selects}},
                           editError, error),
          "timeline placement: " + error);
    const std::string afterPlacement = project.SaveToString();
    ProjectOperation decodedPlacement;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decodedPlacement, editError, error),
          "timeline placement operation round-trip: " + error);
    Check(projectLog.Undo(project, editError, error),
          "undo timeline placement: " + error);
    Check(project.SaveToString() == beforePlacement,
          "bin mutation undo is byte-identical");
    Check(projectLog.Redo(project, editError, error),
          "redo timeline placement: " + error);
    Check(project.SaveToString() == afterPlacement,
          "bin mutation redo is deterministic");
    ProjectEditLog decodedLog;
    Check(ProjectEditLog::Deserialize(projectLog.Serialize(), decodedLog,
                                      editError, error) &&
              decodedLog.AppliedCount() == projectLog.AppliedCount(),
          "project edit log round-trips: " + error);
    Check(project.Validate(error), "project validation: " + error);

    ProjectBinModel model(project);
    const ProjectBinItem* used = model.Find(clip10);
    Check(used && used->total_usage == 1 && used->active_timeline_usage == 1,
          "usage is derived from timelines");

    ProjectBinFilter filter;
    filter.search = "hero";
    auto root = model.FilteredChildren("", filter);
    Check(root.size() == 1 && root.front()->id == selects,
          "a matching descendant keeps its folder hierarchy visible");

    filter = {};
    filter.tags = {"red", "green"};
    filter.ratings = {5};
    filter.usage = ProjectBinUsageFilter::Used;
    auto matches = model.FilteredChildren(interviews, filter);
    Check(matches.size() == 1 && matches.front()->id == clip10,
          "filters are OR within a category and AND across categories");

    filter = {};
    auto sorted = model.FilteredChildren(interviews, filter);
    Check(
        sorted.size() == 2 && sorted[0]->id == clip2 && sorted[1]->id == clip10,
        "name sorting is case-insensitive and numeric-aware");

    const auto selectsChildren = model.Children(selects);
    Check(selectsChildren.size() == 2,
          "folders can contain both folders and timelines");
    std::cout << "project bin tests passed\n";
    return 0;
}

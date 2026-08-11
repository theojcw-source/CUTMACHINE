#include "Project.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

Project::Project(std::string projectName)
    : id(GenerateUlid()), name(std::move(projectName)) {
    DocumentSequence first;
    first.name = "Timeline 1";
    active_timeline_id = first.id;
    timelines.push_back(std::move(first));
}

const DocumentSequence* Project::ActiveTimeline() const {
    return FindTimeline(active_timeline_id);
}

DocumentSequence* Project::ActiveTimeline() {
    return FindTimeline(active_timeline_id);
}

const DocumentSequence* Project::FindTimeline(const Ulid& timelineId) const {
    const auto found = std::find_if(
        timelines.begin(), timelines.end(),
        [&](const DocumentSequence& item) { return item.id == timelineId; });
    return found == timelines.end() ? nullptr : &*found;
}

DocumentSequence* Project::FindTimeline(const Ulid& timelineId) {
    const auto found = std::find_if(
        timelines.begin(), timelines.end(),
        [&](const DocumentSequence& item) { return item.id == timelineId; });
    return found == timelines.end() ? nullptr : &*found;
}

const ProjectBinMetadata* Project::FindBinMetadata(const Ulid& itemId) const {
    const auto found = bin_metadata.find(itemId);
    return found == bin_metadata.end() ? nullptr : &found->second;
}

Document Project::MakeDocument(const Ulid& timelineId) const {
    Document document;
    if (const DocumentSequence* timeline = FindTimeline(timelineId))
        document.sequence = *timeline;
    document.color_management = settings.color_management;
    document.library = rushes;
    document.bins = bins;
    document.sources = sources;
    return document;
}

Document Project::MakeActiveDocument() const {
    return MakeDocument(active_timeline_id);
}

bool Project::CommitDocument(const Ulid& timelineId, const Document& document,
                             std::string& error) {
    if (!document.Validate(error)) return false;
    Project candidate = *this;
    DocumentSequence* timeline = candidate.FindTimeline(timelineId);
    if (!timeline) {
        error = "unknown timeline_id '" + timelineId + "'";
        return false;
    }
    if (document.sequence.id != timeline->id) {
        error =
            "edit document does not belong to timeline '" + timelineId + "'";
        return false;
    }
    *timeline = document.sequence;
    candidate.settings.color_management = document.color_management;
    candidate.rushes = document.library;
    candidate.bins = document.bins;
    candidate.sources = document.sources;
    if (!candidate.Validate(error)) return false;
    *this = std::move(candidate);
    error.clear();
    return true;
}

bool Project::CommitActiveDocument(const Document& document,
                                   std::string& error) {
    return CommitDocument(active_timeline_id, document, error);
}

bool Project::Validate(std::string& error) const {
    if (!IsValidUlid(id)) {
        error = "project has an invalid ID";
        return false;
    }
    if (name.empty()) {
        error = "project has an empty name";
        return false;
    }
    if (timelines.empty()) {
        error = "project has no timelines";
        return false;
    }
    if (!ActiveTimeline()) {
        error = "active_timeline_id does not reference a timeline";
        return false;
    }
    for (const auto& placement : timeline_bin_ids) {
        if (!FindTimeline(placement.first)) {
            error = "bin placement references unknown timeline '" +
                    placement.first + "'";
            return false;
        }
        if (!placement.second.empty() &&
            std::none_of(bins.begin(), bins.end(), [&](const DocumentBin& bin) {
                return bin.id == placement.second;
            })) {
            error =
                "timeline references unknown bin_id '" + placement.second + "'";
            return false;
        }
    }
    for (const auto& entry : bin_metadata) {
        if (entry.second.rating > 5) {
            error = "project bin rating must be between 0 and 5";
            return false;
        }
        if (entry.second.insert_order >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            error = "project bin insertion order is outside int64_t range";
            return false;
        }
        if (!FindTimeline(entry.first) &&
            std::none_of(rushes.begin(), rushes.end(),
                         [&](const LibraryMedia& rush) {
                             return rush.id == entry.first;
                         })) {
            error = "metadata references unknown project bin item '" +
                    entry.first + "'";
            return false;
        }
    }

    std::set<Ulid> timelineObjectIds{id};
    const auto registerSharedId = [&](const Ulid& objectId) {
        if (timelineObjectIds.insert(objectId).second) return true;
        error = "duplicate project object ID '" + objectId + "'";
        return false;
    };
    for (const DocumentBin& bin : bins)
        if (!registerSharedId(bin.id)) return false;
    std::set<Ulid> sourceIds;
    for (const DocumentSource& source : sources) {
        if (!registerSharedId(source.id)) return false;
        sourceIds.insert(source.id);
    }
    for (const LibraryMedia& rush : rushes) {
        // A rush and its mounted source deliberately share one stable ID.
        if (sourceIds.find(rush.id) == sourceIds.end() &&
            !registerSharedId(rush.id))
            return false;
    }
    for (const DocumentSequence& timeline : timelines) {
        Document document;
        document.sequence = timeline;
        document.color_management = settings.color_management;
        document.library = rushes;
        document.bins = bins;
        document.sources = sources;
        if (!document.Validate(error)) {
            error = "timeline '" + timeline.name + "': " + error;
            return false;
        }

        const auto registerId = [&](const Ulid& objectId) {
            return timelineObjectIds.insert(objectId).second;
        };
        if (!registerId(timeline.id)) {
            error =
                "duplicate object ID across timelines '" + timeline.id + "'";
            return false;
        }
        for (const DocumentMarker& marker : timeline.markers) {
            if (!registerId(marker.id)) {
                error =
                    "duplicate object ID across timelines '" + marker.id + "'";
                return false;
            }
        }
        for (const DocumentTransition& transition : timeline.transitions) {
            if (!registerId(transition.id)) {
                error = "duplicate object ID across timelines '" +
                        transition.id + "'";
                return false;
            }
        }
        for (const DocumentTrack& track : timeline.tracks) {
            if (!registerId(track.id)) {
                error =
                    "duplicate object ID across timelines '" + track.id + "'";
                return false;
            }
            for (const DocumentClip& clip : track.clips) {
                if (!registerId(clip.id)) {
                    error = "duplicate object ID across timelines '" + clip.id +
                            "'";
                    return false;
                }
            }
        }
    }
    error.clear();
    return true;
}

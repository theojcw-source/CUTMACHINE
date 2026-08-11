#include "ProjectBin.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace {

std::string Lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ContainsInsensitive(const std::string& value, const std::string& needle) {
    return Lower(value).find(Lower(needle)) != std::string::npos;
}

bool NaturalLess(const std::string& left, const std::string& right) {
    size_t a = 0;
    size_t b = 0;
    while (a < left.size() && b < right.size()) {
        const unsigned char lc = static_cast<unsigned char>(left[a]);
        const unsigned char rc = static_cast<unsigned char>(right[b]);
        if (std::isdigit(lc) && std::isdigit(rc)) {
            size_t ae = a;
            size_t be = b;
            while (ae < left.size() &&
                   std::isdigit(static_cast<unsigned char>(left[ae])))
                ++ae;
            while (be < right.size() &&
                   std::isdigit(static_cast<unsigned char>(right[be])))
                ++be;
            const std::string an = left.substr(a, ae - a);
            const std::string bn = right.substr(b, be - b);
            const size_t az = an.find_first_not_of('0');
            const size_t bz = bn.find_first_not_of('0');
            const std::string av =
                az == std::string::npos ? "0" : an.substr(az);
            const std::string bv =
                bz == std::string::npos ? "0" : bn.substr(bz);
            if (av.size() != bv.size()) return av.size() < bv.size();
            if (av != bv) return av < bv;
            a = ae;
            b = be;
            continue;
        }
        const char al = static_cast<char>(std::tolower(lc));
        const char bl = static_cast<char>(std::tolower(rc));
        if (al != bl) return al < bl;
        ++a;
        ++b;
    }
    return left.size() < right.size();
}

int KindRank(ProjectBinItemKind kind) {
    switch (kind) {
        case ProjectBinItemKind::Folder:
            return 0;
        case ProjectBinItemKind::Timeline:
            return 1;
        case ProjectBinItemKind::Rush:
            return 2;
    }
    return 3;
}

RationalTime SequenceDuration(const DocumentSequence& sequence) {
    RationalTime result{0, 1};
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            const RationalTime end = clip.timeline_in.add(clip.duration);
            if (end > result) result = end;
        }
    }
    return result;
}

}  // namespace

ProjectBinModel::ProjectBinModel(const Project& project) {
    std::map<Ulid, uint32_t> activeUsage;
    std::map<Ulid, uint32_t> totalUsage;
    for (const DocumentSequence& timeline : project.timelines) {
        for (const DocumentTrack& track : timeline.tracks) {
            for (const DocumentClip& clip : track.clips) {
                ++totalUsage[clip.source_id];
                if (timeline.id == project.active_timeline_id)
                    ++activeUsage[clip.source_id];
            }
        }
    }

    for (const DocumentBin& bin : project.bins) {
        items_.push_back(
            {ProjectBinItemKind::Folder, bin.id, bin.parent_id, bin.name});
    }
    for (const LibraryMedia& rush : project.rushes) {
        ProjectBinItem item;
        item.kind = ProjectBinItemKind::Rush;
        item.id = rush.id;
        item.parent_id = rush.bin_id;
        item.name = rush.filename;
        item.duration = rush.duration;
        item.active_timeline_usage = activeUsage[rush.id];
        item.total_usage = totalUsage[rush.id];
        if (const ProjectBinMetadata* metadata =
                project.FindBinMetadata(rush.id)) {
            item.description = metadata->description;
            item.tags = metadata->tags;
            item.rating = metadata->rating;
            item.insert_order = metadata->insert_order;
        }
        items_.push_back(std::move(item));
    }
    for (const DocumentSequence& timeline : project.timelines) {
        ProjectBinItem item;
        item.kind = ProjectBinItemKind::Timeline;
        item.id = timeline.id;
        const auto placement = project.timeline_bin_ids.find(timeline.id);
        if (placement != project.timeline_bin_ids.end())
            item.parent_id = placement->second;
        item.name = timeline.name;
        item.duration = SequenceDuration(timeline);
        if (const ProjectBinMetadata* metadata =
                project.FindBinMetadata(timeline.id)) {
            item.description = metadata->description;
            item.tags = metadata->tags;
            item.rating = metadata->rating;
            item.insert_order = metadata->insert_order;
        }
        items_.push_back(std::move(item));
    }
}

const ProjectBinItem* ProjectBinModel::Find(const Ulid& id) const {
    const auto found =
        std::find_if(items_.begin(), items_.end(),
                     [&](const ProjectBinItem& item) { return item.id == id; });
    return found == items_.end() ? nullptr : &*found;
}

std::vector<const ProjectBinItem*> ProjectBinModel::Children(
    const Ulid& parentId) const {
    std::vector<const ProjectBinItem*> result;
    for (const ProjectBinItem& item : items_)
        if (item.parent_id == parentId) result.push_back(&item);
    return result;
}

bool ProjectBinModel::MatchesSelf(const ProjectBinItem& item,
                                  const ProjectBinFilter& filter) const {
    if (filter.usage == ProjectBinUsageFilter::Used && item.total_usage == 0)
        return false;
    if (filter.usage == ProjectBinUsageFilter::Unused && item.total_usage > 0)
        return false;
    if (!filter.ratings.empty() &&
        std::find(filter.ratings.begin(), filter.ratings.end(), item.rating) ==
            filter.ratings.end())
        return false;
    if (!filter.kinds.empty() &&
        std::find(filter.kinds.begin(), filter.kinds.end(), item.kind) ==
            filter.kinds.end())
        return false;
    if (!filter.tags.empty()) {
        const bool tagMatch =
            std::any_of(filter.tags.begin(), filter.tags.end(),
                        [&](const std::string& wanted) {
                            if (wanted == "#") return item.tags.empty();
                            return std::any_of(
                                item.tags.begin(), item.tags.end(),
                                [&](const std::string& actual) {
                                    return ContainsInsensitive(actual, wanted);
                                });
                        });
        if (!tagMatch) return false;
    }
    if (!filter.search.empty() &&
        !ContainsInsensitive(item.name, filter.search) &&
        !ContainsInsensitive(item.description, filter.search))
        return false;
    return true;
}

bool ProjectBinModel::HasMatchingDescendant(
    const Ulid& id, const ProjectBinFilter& filter) const {
    for (const ProjectBinItem* child : Children(id)) {
        if (MatchesSelf(*child, filter) ||
            HasMatchingDescendant(child->id, filter))
            return true;
    }
    return false;
}

bool ProjectBinModel::Matches(const ProjectBinItem& item,
                              const ProjectBinFilter& filter) const {
    return MatchesSelf(item, filter) || HasMatchingDescendant(item.id, filter);
}

void ProjectBinModel::Sort(std::vector<const ProjectBinItem*>& items,
                           ProjectBinSort sort, bool ascending) const {
    std::stable_sort(items.begin(), items.end(),
                     [&](const ProjectBinItem* a, const ProjectBinItem* b) {
                         // Folders remain before leaf items so the hierarchy
                         // does not jump around when a column changes
                         // direction.
                         if (a->kind != b->kind) {
                             return KindRank(a->kind) < KindRank(b->kind);
                         }
                         bool lessAB = false;
                         bool lessBA = false;
                         switch (sort) {
                             case ProjectBinSort::Duration:
                                 lessAB = a->duration < b->duration;
                                 lessBA = b->duration < a->duration;
                                 break;
                             case ProjectBinSort::Rating:
                                 lessAB = a->rating < b->rating;
                                 lessBA = b->rating < a->rating;
                                 break;
                             case ProjectBinSort::InsertOrder:
                                 lessAB = a->insert_order < b->insert_order;
                                 lessBA = b->insert_order < a->insert_order;
                                 break;
                             case ProjectBinSort::Type:
                             case ProjectBinSort::Name:
                                 lessAB = NaturalLess(a->name, b->name);
                                 lessBA = NaturalLess(b->name, a->name);
                                 break;
                         }
                         if (!lessAB && !lessBA) {
                             lessAB = NaturalLess(a->name, b->name);
                             lessBA = NaturalLess(b->name, a->name);
                         }
                         if (!lessAB && !lessBA) return a->id < b->id;
                         return ascending ? lessAB : lessBA;
                     });
}

std::vector<const ProjectBinItem*> ProjectBinModel::FilteredChildren(
    const Ulid& parentId, const ProjectBinFilter& filter, ProjectBinSort sort,
    bool ascending) const {
    std::vector<const ProjectBinItem*> result;
    for (const ProjectBinItem* item : Children(parentId))
        if (Matches(*item, filter)) result.push_back(item);
    Sort(result, sort, ascending);
    return result;
}

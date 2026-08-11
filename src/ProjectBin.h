#pragma once

#include "Project.h"

#include <cstdint>
#include <string>
#include <vector>

enum class ProjectBinItemKind { Folder, Timeline, Rush };
enum class ProjectBinUsageFilter { All, Used, Unused };
enum class ProjectBinSort { Name, Duration, Rating, Type, InsertOrder };

struct ProjectBinItem {
    ProjectBinItemKind kind = ProjectBinItemKind::Rush;
    Ulid id;
    Ulid parent_id;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    uint32_t rating = 0;
    uint64_t insert_order = 0;
    RationalTime duration{0, 1};
    uint32_t active_timeline_usage = 0;
    uint32_t total_usage = 0;
};

struct ProjectBinFilter {
    std::string search;
    std::vector<std::string> tags;
    std::vector<uint32_t> ratings;
    std::vector<ProjectBinItemKind> kinds;
    ProjectBinUsageFilter usage = ProjectBinUsageFilter::All;
};

// A UI-independent tree/projection of the project's folders, rushes and
// timelines, following Kdenlive's ProjectItemModel + sort/filter proxy split.
class ProjectBinModel {
public:
    explicit ProjectBinModel(const Project& project);

    const ProjectBinItem* Find(const Ulid& id) const;
    std::vector<const ProjectBinItem*> Children(const Ulid& parentId) const;
    std::vector<const ProjectBinItem*> FilteredChildren(
        const Ulid& parentId, const ProjectBinFilter& filter,
        ProjectBinSort sort = ProjectBinSort::Name,
        bool ascending = true) const;
    bool Matches(const ProjectBinItem& item,
                 const ProjectBinFilter& filter) const;

private:
    bool MatchesSelf(const ProjectBinItem& item,
                     const ProjectBinFilter& filter) const;
    bool HasMatchingDescendant(const Ulid& id,
                               const ProjectBinFilter& filter) const;
    void Sort(std::vector<const ProjectBinItem*>& items, ProjectBinSort sort,
              bool ascending) const;

    std::vector<ProjectBinItem> items_;
};

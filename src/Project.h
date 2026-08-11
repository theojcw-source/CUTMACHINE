#pragma once

#include "Document.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct ProjectSettings {
    ColorManagementSettings color_management;
};

struct ProjectBinMetadata {
    std::string description;
    uint32_t rating = 0;
    std::vector<std::string> tags;
    uint64_t insert_order = 0;
};

// User-facing aggregate. Document remains the focused edit snapshot consumed
// by the timeline, renderer, exporter and operation log.
class Project {
public:
    explicit Project(std::string projectName = "Untitled Project");

    Ulid id;
    std::string name;
    ProjectSettings settings;
    std::vector<LibraryMedia> rushes;
    std::vector<DocumentBin> bins;
    std::vector<DocumentSource> sources;
    std::vector<DocumentSequence> timelines;
    Ulid active_timeline_id;
    // Display metadata belongs to the project bin, not to media decoding.
    std::map<Ulid, ProjectBinMetadata> bin_metadata;
    // Timelines are first-class bin items too. Empty means project root.
    std::map<Ulid, Ulid> timeline_bin_ids;

    // Project files wrap one canonical edit document per timeline. Legacy
    // single-Document JSON remains readable and is promoted on load.
    static Project FromDocument(Document document,
                                std::string projectName = "Untitled Project");
    static bool Load(const std::string& path, Project& output,
                     std::string& error);
    static bool LoadFromString(const std::string& json, Project& output,
                               std::string& error);
    bool Save(const std::string& path, std::string& error) const;
    std::string SaveToString() const;

    const DocumentSequence* ActiveTimeline() const;
    DocumentSequence* ActiveTimeline();
    const DocumentSequence* FindTimeline(const Ulid& timelineId) const;
    DocumentSequence* FindTimeline(const Ulid& timelineId);

    const ProjectBinMetadata* FindBinMetadata(const Ulid& itemId) const;

    Document MakeDocument(const Ulid& timelineId) const;
    bool CommitDocument(const Ulid& timelineId, const Document& document,
                        std::string& error);
    Document MakeActiveDocument() const;
    bool CommitActiveDocument(const Document& document, std::string& error);
    bool Validate(std::string& error) const;
};

#pragma once

#include "Operations.h"

#include <cstddef>
#include <string>
#include <vector>

struct Entry {
    Operation op;
    Operation inverse;
};

class EditLog {
public:
    bool Apply(Document& document, Operation operation, EditError& error,
               std::string& message);
    bool Undo(Document& document, EditError& error, std::string& message);
    bool Redo(Document& document, EditError& error, std::string& message);

    size_t AppliedCount() const;
    size_t UndoneCount() const;
    const std::vector<Entry>& AppliedEntries() const;
    const std::vector<Entry>& UndoneEntries() const;

    bool Save(const std::string& path, EditError& error,
              std::string& message) const;
    static bool Load(const std::string& path, EditLog& output, EditError& error,
                     std::string& message);
    std::string Serialize() const;
    static bool Deserialize(const std::string& json, EditLog& output,
                            EditError& error, std::string& message);

private:
    std::vector<Entry> applied_;
    std::vector<Entry> undone_;
};

struct ProjectEntry {
    ProjectOperation op;
    ProjectOperation inverse;
};

class ProjectEditLog {
public:
    bool Apply(Project& project, ProjectOperation operation, EditError& error,
               std::string& message);
    bool ApplyBatch(Project& project, std::vector<ProjectOperation> operations,
                    EditError& error, std::string& message);
    bool Undo(Project& project, EditError& error, std::string& message);
    bool Redo(Project& project, EditError& error, std::string& message);

    size_t AppliedCount() const;
    size_t UndoneCount() const;
    const std::vector<ProjectEntry>& AppliedEntries() const;
    const std::vector<ProjectEntry>& UndoneEntries() const;

    bool Save(const std::string& path, EditError& error,
              std::string& message) const;
    static bool Load(const std::string& path, ProjectEditLog& output,
                     EditError& error, std::string& message);
    std::string Serialize() const;
    static bool Deserialize(const std::string& json, ProjectEditLog& output,
                            EditError& error, std::string& message);

private:
    std::vector<ProjectEntry> applied_;
    std::vector<ProjectEntry> undone_;
};

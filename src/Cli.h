#pragma once

#include "Export.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class Document;
class EditLog;
class ProjectEditLog;
class Project;

// Headless command entry points. These functions depend only on the model
// library and never initialize AppKit, Metal, media decoding, or rendering.
int DescribeCommand(const std::string& documentPath, std::string& output);
int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output);
int ApplyProjectOperationCommand(const std::string& projectPath,
                                 const std::string& operationJson,
                                 std::string& output);
int UndoProjectOperationCommand(const std::string& projectPath,
                                std::string& output);
int RedoProjectOperationCommand(const std::string& projectPath,
                                std::string& output);

// Timeline-level undo/redo counterparts to ApplyOperationCommand. Operate on
// the active timeline's own EditLog, the same log --apply-op appends to, so
// an MCP undo/redo tool call is indistinguishable on disk from a human
// triggering undo/redo in the app. There is no CLI flag for these yet; they
// exist so the MCP server (Operations.h ticket F1.1) can reuse this exact
// path instead of duplicating EditLog::Undo/Redo call sites.
int UndoOperationCommand(const std::string& documentPath, std::string& output);
int RedoOperationCommand(const std::string& documentPath, std::string& output);
int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output);

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId);
std::string ProjectEditLogPathForProject(const std::string& projectPath);

// Transactional primitive for storage layouts that add package-local
// artifacts. All destinations must be regular files on the same filesystem.
bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message);

#pragma once

#include "Export.h"

#include <string>

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
int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output);

std::string EditLogPathForDocument(const std::string& documentPath);
std::string ProjectEditLogPathForProject(const std::string& projectPath);

// Shared transactional persistence used by both the headless command and the
// graphical editor after an EditLog operation succeeds.
bool CommitDocumentAndEditLog(const std::string& documentPath,
                              const Document& document, const EditLog& log,
                              std::string& message);
bool CommitProjectAndEditLog(const std::string& projectPath,
                             const Project& project, const EditLog& log,
                             std::string& message);
bool CommitProjectAndLogs(const std::string& projectPath,
                          const Project& project, const EditLog& timelineLog,
                          const ProjectEditLog& projectLog,
                          std::string& message);

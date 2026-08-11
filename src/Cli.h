#pragma once

#include "Export.h"

#include <string>

class Document;
class EditLog;

// Headless command entry points. These functions depend only on the model
// library and never initialize AppKit, Metal, media decoding, or rendering.
int DescribeCommand(const std::string& documentPath, std::string& output);
int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output);
int ExportCommand(const std::string& documentPath,
                  const ExportSettings& settings,
                  const ExportProgressCallback& progress,
                  const std::atomic_bool* cancel, std::string& output);

std::string EditLogPathForDocument(const std::string& documentPath);

// Shared transactional persistence used by both the headless command and the
// graphical editor after an EditLog operation succeeds.
bool CommitDocumentAndEditLog(const std::string& documentPath,
                              const Document& document, const EditLog& log,
                              std::string& message);

#pragma once

#include <string>

// Scans media headers only. No decoder, renderer, AppKit, or Metal object is
// created by this command.
int IngestCommand(const std::string& documentPath,
                  const std::string& directoryPath, bool recursive,
                  std::string& output);

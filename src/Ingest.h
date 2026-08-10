#pragma once

#include "Document.h"

#include <string>

// Reads container/stream headers without decoding frames. The caller owns
// identity/path fields; technical metadata is filled on success.
bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason);

// Scans media headers only. No decoder, renderer, AppKit, or Metal object is
// created by this command.
int IngestCommand(const std::string& documentPath,
                  const std::string& directoryPath, bool recursive,
                  std::string& output);

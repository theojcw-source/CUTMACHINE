#pragma once

#include "Document.h"

#include <filesystem>
#include <string>

// ALPHA-2026-08 -- Read-only semantic view for agent-driven interview edits.
// Every selectable span carries source-exact times copied from the transcript;
// the model selects ranges but never computes an edit boundary.
bool DescribeTimelineTranscriptForAgent(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::string& json, std::string& error);

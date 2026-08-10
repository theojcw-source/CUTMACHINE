#pragma once

#include <string>

// Headless command entry points. These functions depend only on the model
// library and never initialize AppKit, Metal, media decoding, or rendering.
int DescribeCommand(const std::string& documentPath, std::string& output);
int ApplyOperationCommand(const std::string& documentPath,
                          const std::string& operationJson,
                          std::string& output);

std::string EditLogPathForDocument(const std::string& documentPath);

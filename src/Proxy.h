#pragma once

#include "Document.h"
#include "MediaTaskManager.h"

#include <string>

struct ProxySettings {
    int32_t max_width = 1280;
    std::string ffmpeg_path = "ffmpeg";
};

// Generates a silent, intraframe ProRes Proxy while retaining the source
// cadence. Output replacement is atomic and partial files are removed.
bool GenerateMediaProxy(const std::string& inputPath,
                        const std::string& outputPath,
                        const RationalTime& duration,
                        const ProxySettings& settings,
                        MediaTaskContext& context, std::string& error);

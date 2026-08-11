#pragma once

#include "Document.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class ExportEncoder {
    HevcVideoToolbox,
    HevcSoftware,
};

enum class ExportPresetId {
    HevcHighQuality,
    HevcMaster,
    HevcWeb1080,
    HevcUhdDelivery,
};

struct ExportPresetDescriptor {
    ExportPresetId id;
    const char* name;
    const char* summary;
};

struct ExportSettings {
    std::string output_path;
    int32_t width = 1920;
    int32_t height = 1080;
    MediaRate frame_rate{25, 1};
    ExportEncoder encoder = ExportEncoder::HevcVideoToolbox;
    int64_t video_bitrate = 45000000;
    int32_t audio_bitrate = 320000;
    int32_t audio_sample_rate = 48000;
    bool main10 = true;
    bool overwrite = false;
    std::string ffmpeg_path = "ffmpeg";
};

struct ExportProgress {
    int64_t rendered_frames = 0;
    int64_t total_frames = 0;
    int64_t output_time_us = 0;
    double speed = 0.0;
    double fraction = 0.0;
};

struct ExportPlan {
    ExportSettings settings;
    RationalTime duration{0, 1};
    int64_t total_frames = 0;
    std::string filter_graph;
    std::vector<std::string> arguments;
    std::string temporary_output_path;
    std::string temporary_lut_path;
    std::string color_lut;
};

using ExportProgressCallback = std::function<void(const ExportProgress&)>;

// Offline export is deliberately isolated from the realtime player. A plan is
// immutable and inspectable, so future Metal and distributed render backends
// can consume the same validated settings without changing the UI or presets.
class Exporter {
public:
    static const std::vector<ExportPresetDescriptor>& Presets();
    static ExportSettings SettingsForPreset(ExportPresetId preset,
                                            const std::string& outputPath,
                                            int32_t sourceWidth,
                                            int32_t sourceHeight,
                                            MediaRate sourceRate);
    static ExportSettings HevcMain10Preset(const std::string& outputPath);
    static ExportSettings HevcMain10SoftwarePreset(
        const std::string& outputPath);

    static bool BuildPlan(const Document& document,
                          const std::string& documentPath,
                          const ExportSettings& settings, ExportPlan& output,
                          std::string& error);

    // Runs synchronously. Callers that need a responsive UI should run this on
    // a worker queue. Setting cancel to true terminates FFmpeg and removes the
    // incomplete temporary file; the destination is only replaced on success.
    static bool Run(const ExportPlan& plan,
                    const ExportProgressCallback& progress,
                    const std::atomic_bool* cancel, std::string& error);
};

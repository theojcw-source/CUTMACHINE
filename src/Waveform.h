#pragma once

#include "MediaTaskManager.h"
#include "RationalTime.h"

#include <cstdint>
#include <string>
#include <vector>

struct AudioWaveform {
    uint32_t peaks_per_second = 100;
    std::vector<float> peaks;
};

struct WaveformSettings {
    uint32_t decode_sample_rate = 8000;
    uint32_t peaks_per_second = 100;
    std::string ffmpeg_path = "ffmpeg";
};

bool GenerateAudioWaveform(const std::string& inputPath,
                           const std::string& outputPath,
                           const RationalTime& duration,
                           const WaveformSettings& settings,
                           MediaTaskContext& context, std::string& error);

bool LoadAudioWaveform(const std::string& path, AudioWaveform& waveform,
                       std::string& error);

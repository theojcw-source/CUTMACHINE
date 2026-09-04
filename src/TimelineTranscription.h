#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "Transcription.h"

#include <filesystem>
#include <string>
#include <vector>

// B7 -- ROADMAP.md. The exporter's audio filter graph is the authoritative
// description of what a timeline sounds like. This plan keeps the analogous
// decode route inspectable and lets transcription consume its PCM directly,
// never through a caller-visible movie or temporary project.
struct TimelineAudioPlan {
    std::vector<std::string> ffmpeg_arguments;
    RationalTime duration{0, 1};
    size_t audio_clips = 0;
    // Changes whenever an audible clip's source boundaries or placement
    // changes. It is stored as Transcript::media_id, alongside model and
    // language, so a stale timeline transcript is never reused.
    std::string cache_identity;
};

bool BuildTimelineAudioPlan(const Document& document,
                            const std::filesystem::path& projectPath,
                            TimelineAudioPlan& plan, std::string& error);

// Transcribes the audible audio arrangement, atomically caching the normal
// Transcript under `cachePath`. Cache identity includes the exact audio edit;
// settings include model, language and verbatim mode.
bool TranscribeTimelineAudio(const Document& document,
                             const std::filesystem::path& projectPath,
                             const std::filesystem::path& cachePath,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, Transcript& transcript,
                             bool& cacheHit, std::string& error);

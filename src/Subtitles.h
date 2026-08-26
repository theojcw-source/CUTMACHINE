#pragma once

#include "Document.h"
#include "Operations.h"
#include "Transcription.h"

#include <string>
#include <vector>

// SRT-2026-08 -- SubRip is a surface format. The engine keeps its timestamps
// exact and turns one import into the existing reversible AddTrack operation.
struct SubtitleCue {
    RationalTime timeline_in{0, 1000};
    RationalTime duration{0, 1000};
    std::string text;
};

bool ParseSrt(const std::string& contents, std::vector<SubtitleCue>& cues,
              std::string& error);
bool LoadSrt(const std::string& path, std::vector<SubtitleCue>& cues,
             std::string& error);
bool SaveSrt(const Document& document, const std::string& path,
             std::string& error);

AddTrackOperation BuildSubtitleTrackEdit(const std::vector<SubtitleCue>& cues,
                                         int32_t trackIndex,
                                         const Ulid& trackId = {});

std::vector<const DocumentClip*> ActiveSubtitles(const Document& document,
                                                 RationalTime position);

// F1.4 -- Word timestamps are source-relative; this deterministic bridge maps
// them through a clip and groups them into readable timeline-relative cues.
bool SubtitleCuesForClip(const Transcript& transcript, const DocumentClip& clip,
                         std::vector<SubtitleCue>& cues, std::string& error);

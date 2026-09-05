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

// The silence that ends one cue and starts the next. SubtitleCuesForClip
// breaks a cue when words are further apart than this, so two consecutive
// cues are separated either by nothing, or by a pause shorter than this that
// simply falls outside both of them. Exposed because InterviewShort.cc has
// to tell those two apart from a real editorial gap.
inline constexpr RationalTime kSubtitleCueMaximumGap{7, 10};

bool ParseSrt(const std::string& contents, std::vector<SubtitleCue>& cues,
              std::string& error);
bool LoadSrt(const std::string& path, std::vector<SubtitleCue>& cues,
             std::string& error);
bool SaveSrt(const Document& document, const std::string& path,
             std::string& error);

// SRT-2026-08 -- the serializer SaveSrt is built on, taking cues directly.
// A caller that already holds them from somewhere other than a caption track
// -- the timeline's cached transcripts, say -- can write a file without first
// mutating the document to park them on a track it does not otherwise want.
bool WriteSrt(const std::vector<SubtitleCue>& cues, const std::string& path,
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

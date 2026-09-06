#pragma once

#include "Document.h"

#include <cstdint>
#include <optional>
#include <vector>

struct ResolvedFrame {
    Ulid source_id;
    int64_t source_frame = 0;
    // The DocumentClip this frame came from. Render-time color grading
    // (F1.3, see ColorEffects.h) reads this clip's `effects` stack; nothing
    // else in Timeline uses clip identity, so this stays a plain lookup key
    // rather than a reference into the document.
    Ulid clip_id;
};

struct ResolvedLayer {
    ResolvedFrame frame;
    float opacity = 1.0f;
};

struct TrackResolution {
    Ulid track_id;
    std::optional<ResolvedFrame> frame;
};

class Timeline {
public:
    explicit Timeline(const Document& document);

    std::vector<TrackResolution> Resolve(RationalTime position) const;
    std::optional<ResolvedFrame> ResolveTrack(const Ulid& trackId,
                                              RationalTime position) const;
    // Bottom-to-top layers. A normal edit returns one opaque layer; an active
    // dissolve returns the outgoing frame plus the incoming frame opacity.
    std::vector<ResolvedLayer> ResolveTrackLayers(const Ulid& trackId,
                                                  RationalTime position) const;
    // PERF-2026-09. The first frame playback will need on the far side of the
    // nearest cut in each track, at most `lookahead` away from `position` in
    // the direction of travel (`direction` positive plays forward, negative
    // backward). Asking a decoder for that frame only once the playhead
    // reaches the cut is too late: the incoming layer composites as a hole
    // for as long as the decode takes, and the track underneath flashes
    // through. One entry per visible video track that has such a cut -- the
    // tracks that feed the composite -- and no entry for the others.
    std::vector<ResolvedFrame> ResolveUpcoming(RationalTime position,
                                               int direction,
                                               RationalTime lookahead) const;
    RationalTime Duration() const;

private:
    std::optional<ResolvedFrame> ResolveInTrack(const DocumentTrack& track,
                                                RationalTime position) const;
    std::optional<ResolvedFrame> ResolveUpcomingInTrack(
        const DocumentTrack& track, RationalTime position, int direction,
        RationalTime lookahead) const;
    ResolvedFrame ResolveClipAt(const DocumentClip& clip,
                                RationalTime sourceTime) const;
    int64_t LastSourceFrame(const DocumentClip& clip) const;

    const Document& document_;
};

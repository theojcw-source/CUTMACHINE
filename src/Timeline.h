#pragma once

#include "Document.h"

#include <cstdint>
#include <optional>
#include <vector>

struct ResolvedFrame {
    Ulid source_id;
    int64_t source_frame = 0;
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
    RationalTime Duration() const;

private:
    std::optional<ResolvedFrame> ResolveInTrack(const DocumentTrack& track,
                                                RationalTime position) const;

    const Document& document_;
};

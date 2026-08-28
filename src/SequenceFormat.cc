#include "SequenceFormat.h"

#include <algorithm>
#include <sstream>

namespace {

// Compares rates without dividing: a/b > c/d becomes a*d > c*b. The operands
// are stream rates, so the products stay far inside int64_t.
bool RateGreater(const MediaRate& left, const MediaRate& right) {
    return static_cast<int64_t>(left.num) * right.den >
           static_cast<int64_t>(right.num) * left.den;
}

bool RateEqual(const MediaRate& left, const MediaRate& right) {
    return static_cast<int64_t>(left.num) * right.den ==
           static_cast<int64_t>(right.num) * left.den;
}

bool SameFormat(const SequenceFormatCandidate& candidate,
                int32_t width, int32_t height, const MediaRate& rate) {
    return candidate.width == width && candidate.height == height &&
           candidate.frame_rate.num == rate.num &&
           candidate.frame_rate.den == rate.den;
}

// Strict weak ordering used to rank candidates: most media first, then the
// larger frame, the higher rate, and finally the raw fields. Every step is
// needed for the result to be independent of library order.
bool Better(const SequenceFormatCandidate& left,
            const SequenceFormatCandidate& right) {
    if (left.media_count != right.media_count)
        return left.media_count > right.media_count;
    const int64_t leftArea = static_cast<int64_t>(left.width) * left.height;
    const int64_t rightArea = static_cast<int64_t>(right.width) * right.height;
    if (leftArea != rightArea) return leftArea > rightArea;
    if (!RateEqual(left.frame_rate, right.frame_rate))
        return RateGreater(left.frame_rate, right.frame_rate);
    if (left.width != right.width) return left.width > right.width;
    if (left.height != right.height) return left.height > right.height;
    if (left.frame_rate.num != right.frame_rate.num)
        return left.frame_rate.num > right.frame_rate.num;
    return left.frame_rate.den < right.frame_rate.den;
}

}  // namespace

bool DisplayDimensions(const LibraryMedia& media, int32_t& width,
                       int32_t& height) {
    if (media.width <= 0 || media.height <= 0) return false;
    // FFmpeg reports a counterclockwise angle that can be negative or beyond
    // a full turn; normalize into [0, 360) before asking whether it is a
    // quarter turn.
    int32_t rotation = media.rotation_degrees % 360;
    if (rotation < 0) rotation += 360;
    if (rotation % 90 != 0) return false;
    if (rotation == 90 || rotation == 270) {
        width = media.height;
        height = media.width;
    } else {
        width = media.width;
        height = media.height;
    }
    return true;
}

bool ResolveSequenceFormat(const std::vector<LibraryMedia>& library,
                           SequenceFormatProposal& proposal,
                           std::string& error) {
    SequenceFormatProposal built;
    for (const LibraryMedia& media : library) {
        int32_t width = 0;
        int32_t height = 0;
        if (!DisplayDimensions(media, width, height) || media.rate.num <= 0 ||
            media.rate.den <= 0) {
            ++built.media_ignored;
            continue;
        }
        ++built.media_considered;
        const auto existing = std::find_if(
            built.candidates.begin(), built.candidates.end(),
            [&](const SequenceFormatCandidate& candidate) {
                return SameFormat(candidate, width, height, media.rate);
            });
        if (existing != built.candidates.end()) {
            ++existing->media_count;
            continue;
        }
        built.candidates.push_back({width, height, media.rate, 1});
    }

    if (built.candidates.empty()) {
        error =
            "no media in this project carries a usable picture format: a "
            "sequence cannot be derived from audio-only or unreadable media";
        return false;
    }
    std::stable_sort(built.candidates.begin(), built.candidates.end(), Better);
    built.chosen = built.candidates.front();
    built.unanimous = built.candidates.size() == 1;
    proposal = std::move(built);
    error.clear();
    return true;
}

std::string SequenceFormatProposalJson(const SequenceFormatProposal& proposal,
                                       const std::string& extraFields) {
    std::ostringstream output;
    const auto candidateJson = [](const SequenceFormatCandidate& candidate) {
        std::ostringstream item;
        item << "{\"width\":" << candidate.width
             << ",\"height\":" << candidate.height
             << ",\"frame_rate\":{\"num\":" << candidate.frame_rate.num
             << ",\"den\":" << candidate.frame_rate.den
             << "},\"media_count\":" << candidate.media_count << "}";
        return item.str();
    };
    output << "{\"ok\":true,\"chosen\":" << candidateJson(proposal.chosen)
           << ",\"unanimous\":" << (proposal.unanimous ? "true" : "false")
           << ",\"media_considered\":" << proposal.media_considered
           << ",\"media_ignored\":" << proposal.media_ignored
           << ",\"candidates\":[";
    for (size_t index = 0; index < proposal.candidates.size(); ++index) {
        if (index) output << ',';
        output << candidateJson(proposal.candidates[index]);
    }
    output << ']' << extraFields << '}';
    return output.str();
}

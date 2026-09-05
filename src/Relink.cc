#include "Relink.h"

#include <cstdint>
#include <utility>

bool ValidateRelinkCandidate(const Document& document, const Ulid& mediaId,
                             const LibraryMedia& replacement,
                             std::string& error) {
    error.clear();
    const DocumentSource* source = document.FindSource(mediaId);
    const LibraryMedia* media = document.FindLibraryMedia(mediaId);
    if (!source || !media) {
        error = "unknown media or mounted source";
        return false;
    }
    if (!replacement.metadata_complete ||
        (media->has_video && !replacement.has_video) ||
        (!media->has_video && !replacement.has_audio)) {
        error = media->has_video
                    ? "replacement has no video for existing video media"
                    : "replacement has no audio for existing audio media";
        return false;
    }
    if (static_cast<int64_t>(replacement.rate.num) * source->rate.den !=
        static_cast<int64_t>(source->rate.num) * replacement.rate.den) {
        error = "replacement frame rate differs from the original";
        return false;
    }
    if (replacement.duration < source->duration) {
        error = "replacement is shorter than the original source";
        return false;
    }
    bool audioRequired = false;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips)
            if (clip.source_id == mediaId) audioRequired = true;
    }
    if (audioRequired && !replacement.has_audio) {
        error = "replacement has no audio for existing audio clips";
        return false;
    }
    return true;
}

#pragma once

#include "RationalTime.h"
#include "Ulid.h"

#include <cstdint>
#include <string>
#include <vector>

struct MediaRate {
    int32_t num = 0;
    int32_t den = 1;
};

struct DocumentSource {
    Ulid id = GenerateUlid();
    std::string path;
    MediaRate rate;
    RationalTime duration;
};

struct LibraryMedia {
    Ulid id = GenerateUlid();
    std::string path;
    std::string filename;
    std::string codec;
    int32_t width = 0;
    int32_t height = 0;
    // Counterclockwise display rotation reported by FFmpeg's display matrix.
    int32_t rotation_degrees = 0;
    MediaRate rate;
    RationalTime duration;
    std::string orientation;
    bool has_audio = false;
    int32_t audio_rate = 0;
    int32_t audio_channels = 0;
    // Empty means the project root. Bins organize library media only and do
    // not change source or clip identity.
    Ulid bin_id;

    // Version-1 sources do not contain technical metadata. They are promoted
    // to the library on load without fabricating values; a later ingest of the
    // same path replaces the incomplete entry with probed metadata.
    bool metadata_complete = true;
};

struct DocumentBin {
    Ulid id = GenerateUlid();
    std::string name;
    // Empty means a top-level bin. Bins form a project-local hierarchy.
    Ulid parent_id;
};

struct DocumentClip {
    Ulid id = GenerateUlid();
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    // Video clips contribute embedded audio until DetachAudio creates an
    // independent audio-track clip and clears this flag.
    bool include_audio = true;
    // Clips produced by one A/V separation share this stable group. It is a
    // selection relationship only; their edit times remain independent.
    Ulid link_group_id;
    // Exact phase relationship captured when the link is created. Drift is
    // (timeline_in-source_in) relative to the anchor, minus this reference.
    Ulid sync_anchor_clip_id;
    RationalTime sync_reference_delta{0, 1};
};

struct DocumentTrack {
    Ulid id = GenerateUlid();
    std::string kind;
    int32_t index = 0;
    std::vector<DocumentClip> clips;
};

class Document {
public:
    int32_t version = 2;
    std::vector<LibraryMedia> library;
    std::vector<DocumentBin> bins;
    std::vector<DocumentSource> sources;
    std::vector<DocumentTrack> tracks;

    static bool Load(const std::string& path, Document& output,
                     std::string& error);
    static bool LoadFromString(const std::string& json, Document& output,
                               std::string& error);
    bool Save(const std::string& path, std::string& error) const;
    std::string SaveToString() const;
    bool Validate(std::string& error) const;

    const DocumentSource* FindSource(const Ulid& id) const;
    DocumentSource* FindSource(const Ulid& id);
    const LibraryMedia* FindLibraryMedia(const Ulid& id) const;
    LibraryMedia* FindLibraryMedia(const Ulid& id);
    const DocumentBin* FindBin(const Ulid& id) const;
    DocumentBin* FindBin(const Ulid& id);
    const DocumentTrack* FindTrack(const Ulid& id) const;
    DocumentTrack* FindTrack(const Ulid& id);
    const DocumentClip* FindClip(const Ulid& id) const;
    DocumentClip* FindClip(const Ulid& id);
    const DocumentTrack* FindTrackForClip(const Ulid& clipId) const;
    DocumentTrack* FindTrackForClip(const Ulid& clipId);
};

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

// Project-wide display pipeline. Values are deliberately named (rather than
// opaque numeric IDs) so project files remain readable and extensible.
struct ColorManagementSettings {
    bool enabled = false;
    std::string input_gamut = "rec709";
    std::string input_transfer = "rec709";
    std::string input_ycbcr_matrix = "auto";
    std::string input_range = "auto";
    std::string working_gamut = "acescct";
    std::string output_gamut = "rec709";
    std::string output_transfer = "rec709";
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
    std::string pixel_format;
    std::string color_range;
    std::string color_space;
    std::string color_transfer;
    std::string color_primaries;
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

// A project marker is addressable independently from clips and tracks. Its
// position remains exact in the document time domain; color and category stay
// textual so project files remain readable and extensible.
struct DocumentMarker {
    Ulid id = GenerateUlid();
    std::string name;
    RationalTime time;
    std::string color = "#f5c542";
    std::string category = "note";
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

// A sequence is the addressable owner of one timeline. Project media and bins
// remain outside it; tracks and sequence markers cannot exist without it.
struct DocumentSequence {
    Ulid id = GenerateUlid();
    std::string name = "Sequence 1";
    int32_t width = 1920;
    int32_t height = 1080;
    MediaRate frame_rate{25, 1};
    std::vector<DocumentMarker> markers;
    std::vector<DocumentTrack> tracks;
};

class Document {
public:
    int32_t version = 3;
    DocumentSequence sequence;
    ColorManagementSettings color_management;
    std::vector<LibraryMedia> library;
    std::vector<DocumentBin> bins;
    std::vector<DocumentSource> sources;

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
    const DocumentMarker* FindMarker(const Ulid& id) const;
    DocumentMarker* FindMarker(const Ulid& id);
    const DocumentTrack* FindTrack(const Ulid& id) const;
    DocumentTrack* FindTrack(const Ulid& id);
    const DocumentClip* FindClip(const Ulid& id) const;
    DocumentClip* FindClip(const Ulid& id);
    const DocumentTrack* FindTrackForClip(const Ulid& clipId) const;
    DocumentTrack* FindTrackForClip(const Ulid& clipId);
};

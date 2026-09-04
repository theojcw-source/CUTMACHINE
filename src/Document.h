#pragma once

#include "RationalTime.h"
#include "Ulid.h"

#include <cstdint>
#include <map>
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

// QC-2026-09 A3 -- audio levels carried as integers: an RMS amplitude as a
// fraction of full scale, times this scale. Amplitude rather than decibels,
// and integer rather than float, for the reason SpeechOnset.h's identical
// convention gives -- a logarithm cannot be taken exactly in integers, and
// no float literal may reach a canonical document. -60 dBFS is 1000 here,
// -74 dBFS (a silent cutaway, measured) is 200, ordinary speech is tens of
// thousands.
constexpr int64_t kAudioLevelScale = 1000000;

// Below this, a media carries no speech worth transcribing: it is a mute
// cutaway, and Whisper on it costs eleven times its own runtime to produce
// nothing. -60 dBFS, which sits far above the -74 dBFS the silent rushes of
// ADS213_ITW_Findetudefevr26 measured and far below any usable dialogue.
constexpr int64_t kSilentMediaAudioLevel = 1000;

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
    // B11 -- ROADMAP.md. Audio-only media remains first-class library media;
    // consumers that decode pictures must check this capability explicitly.
    // True by default keeps pre-B11 documents and hand-built video fixtures
    // source-compatible when their serialized entry has no has_video field.
    bool has_video = true;
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
    // QC-2026-09 A3 -- mean level of the whole audio stream, in the scale
    // above. A document fact like the frame rate and the duration, and
    // recorded for the same reason: it is a property of the file that every
    // surface needs and nobody should have to measure twice. Measured at
    // ingest, which is why it is here and not in a cache -- a cache entry
    // would be one more thing to be missing at the moment a decision depends
    // on it.
    //
    // `audio_level_measured` distinguishes "not measured" from "digital
    // silence", which are the same zero and are not the same fact: one is an
    // unknown a caller can act on, the other is an answer.
    bool audio_level_measured = false;
    int64_t audio_level = 0;
    // Empty means the project root. Bins organize library media only and do
    // not change source or clip identity.
    Ulid bin_id;
    // Derived video-only media used for interactive decoding. Export and
    // audio always resolve the original path above.
    std::string proxy_path;

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

// An exact fraction for a continuous effect knob (exposure amount, mix,
// etc.). Values stay num/den pairs for the same reason RationalTime never
// carries a float: the canonical JSON parser rejects floating-point number
// literals outright, and a knob dragged in the UI must still read back the
// same bytes it was saved with. F1.3 is the single explicit boundary where
// this becomes a float for the Metal kernel that consumes it.
struct EffectParamValue {
    int32_t num = 0;
    int32_t den = 1;
};

// One stage in a clip's ordered color-grade stack. `type` is a dotted knob
// family ("color.exposure", "color.contrast", "color.saturation", ...);
// `params` holds its numeric knobs. Restricted to the "color.*" family for
// now (see ROADMAP.md F0.1); F1.3 interprets these into Metal filters.
struct ClipEffect {
    Ulid id = GenerateUlid();
    std::string type;
    std::map<std::string, EffectParamValue> params;
};

struct DocumentClip {
    Ulid id = GenerateUlid();
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    // Deprecated migration flag. New video clips are always silent; audio is
    // represented exclusively by clips on audio tracks.
    bool include_audio = true;
    // Clips produced by one A/V separation share this stable group. It is a
    // selection relationship only; their edit times remain independent.
    Ulid link_group_id;
    // Exact phase relationship captured when the link is created. Drift is
    // (timeline_in-source_in) relative to the anchor, minus this reference.
    Ulid sync_anchor_clip_id;
    RationalTime sync_reference_delta{0, 1};
    // Ordered color-grade stack. Video-track clips only; see ClipEffect.
    std::vector<ClipEffect> effects;
    // Clips that make up one on-screen caption run share this ID, which must
    // reference a DocumentSequence::caption_styles entry. Empty means the
    // clip carries no caption.
    Ulid caption_group_id;
    // This clip's slice of the caption run's text.
    std::string caption_text;
    // ALPHA-2026-08 -- compositing changes the rendered project, so opacity
    // belongs to the canonical clip rather than to an Inspector preference.
    // Keep it exact until Timeline resolves the render-only float boundary.
    EffectParamValue opacity{1, 1};
    // QC-2026-09 A7 -- audio processing is part of the edit, not a mixer
    // preference: a quiet sentence must export and play back identically on
    // every machine. Gain is an exact number of decibels; the two envelopes
    // are exact timeline durations, never sample counts guessed by a UI.
    EffectParamValue audio_gain_db{0, 1};
    RationalTime audio_fade_in{0, 1};
    RationalTime audio_fade_out{0, 1};
};

enum class TransitionAlignment { Center, StartAtCut, EndAtCut };

// A transition belongs to the sequence, never to either clip. The referenced
// clips keep their original, non-overlapping edit rectangles while the
// transition reads the media handles around their shared cut.
struct DocumentTransition {
    Ulid id = GenerateUlid();
    Ulid track_id;
    Ulid left_clip_id;
    Ulid right_clip_id;
    std::string type = "cross_dissolve";
    RationalTime duration;
    TransitionAlignment alignment = TransitionAlignment::Center;
};

// Shared presentation for one caption run. Clips join the run by setting
// their caption_group_id to this style's id; the text itself stays per-clip
// on DocumentClip so a run can span several underlying media clips.
struct CaptionStyle {
    Ulid id = GenerateUlid();
    std::string font_family = "system";
    int32_t font_size = 48;
    std::string color = "#ffffff";
    std::string position = "bottom";
};

// One camera's take within a multicam group. The angle's alignment to the
// other angles is not duplicated here: it lives on the referenced clip's
// existing sync_anchor_clip_id/sync_reference_delta (see DocumentClip),
// the same primitive used for ordinary A/V link groups.
struct MulticamAngle {
    Ulid id = GenerateUlid();
    std::string name;
    Ulid clip_id;
};

struct MulticamGroup {
    Ulid id = GenerateUlid();
    std::string name = "Multicam 1";
    std::vector<MulticamAngle> angles;
    // Empty means no angle is currently selected; otherwise references one
    // of angles[].id.
    Ulid active_angle_id;
};

struct DocumentTrack {
    Ulid id = GenerateUlid();
    std::string kind;
    int32_t index = 0;
    std::vector<DocumentClip> clips;
    // Locked tracks remain readable but reject edit operations.
    bool locked = false;
    // Ripple edits initiated on another track affect this track when enabled.
    // This is independent from the hard edit lock above.
    bool sync_lock = true;
    // Output state is canonical because it changes playback and export, not
    // merely local UI chrome. Video uses visible; audio uses muted/solo.
    bool visible = true;
    bool muted = false;
    bool solo = false;
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
    std::vector<DocumentTransition> transitions;
    std::vector<DocumentTrack> tracks;
    std::vector<CaptionStyle> caption_styles;
    std::vector<MulticamGroup> multicam_groups;
};

class Document {
public:
    int32_t version = 7;
    DocumentSequence sequence;
    ColorManagementSettings color_management;
    std::vector<LibraryMedia> library;
    std::vector<DocumentBin> bins;
    std::vector<DocumentSource> sources;

    static bool Load(const std::string& path, Document& output,
                     std::string& error);
    static bool LoadFromString(const std::string& json, Document& output,
                               std::string& error);
    static bool LoadFromString(const std::string& json, Document& output,
                               std::string& error, bool validate);
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
    const DocumentTransition* FindTransition(const Ulid& id) const;
    DocumentTransition* FindTransition(const Ulid& id);
    const CaptionStyle* FindCaptionStyle(const Ulid& id) const;
    CaptionStyle* FindCaptionStyle(const Ulid& id);
    const MulticamGroup* FindMulticamGroup(const Ulid& id) const;
    MulticamGroup* FindMulticamGroup(const Ulid& id);
    const DocumentTrack* FindTrack(const Ulid& id) const;
    DocumentTrack* FindTrack(const Ulid& id);
    const DocumentClip* FindClip(const Ulid& id) const;
    DocumentClip* FindClip(const Ulid& id);
    const DocumentTrack* FindTrackForClip(const Ulid& clipId) const;
    DocumentTrack* FindTrackForClip(const Ulid& clipId);
};

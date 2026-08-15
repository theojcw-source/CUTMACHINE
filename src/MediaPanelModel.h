#pragma once

// F2.3 -- ROADMAP.md: view-model logic for the Media panel's three tabs
// (Media / Audio / Captions). Plain C++, no AppKit -- everything here is a
// pure function of Document.h/Operations.h types, so it is directly
// unit-testable (see tests/media_panel_model_tests.cc) without a macOS
// toolchain, the same way UiTheme.h and PanelLayout.h are (see their
// respective tests).
//
// This header holds *only* things that are safe to compute without a
// document mutation: filtering/searching the existing library, describing
// existing caption styles, and building the exact Operations.h structs a
// caller then applies via EditLog::Apply. It never mutates a Document and
// never invents new document/operation state -- see ROADMAP.md F2.3's hard
// constraints and PHILOSOPHY.md principle 2. main.mm (AppKit, unverified
// here) is the only place that actually calls EditLog::Apply.

#include "Document.h"
#include "Operations.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace ui::media_panel {

// The Media panel's three tabs, in display order. Unlike PanelLayout.h's
// PanelSlot (one slot per *dock*, switched via CMPanelHostView), these are
// sub-tabs *within* the single PanelSlot::Media slot -- see PanelLayout.h's
// file comment on why F2.3 does not need CMPanelHostView's dock-level tab
// chrome. A local NSView-based tab strip (CMTabStripView, UiComponents.h)
// switches between them; nothing here is ever serialized.
enum class Tab { Media, Audio, Captions };

inline const std::array<Tab, 3>& AllTabs() {
    static const std::array<Tab, 3> kTabs{Tab::Media, Tab::Audio,
                                          Tab::Captions};
    return kTabs;
}

// Matches the app's French UI (see PanelLayout.h's panel titles).
inline const char* TabTitle(Tab tab) {
    switch (tab) {
        case Tab::Media:
            return "Média";
        case Tab::Audio:
            return "Audio";
        case Tab::Captions:
            return "Légendes";
    }
    return "?";
}

namespace detail {

inline std::string Lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool ContainsInsensitive(const std::string& haystack,
                                const std::string& needleLower) {
    if (needleLower.empty()) return true;
    return Lower(haystack).find(needleLower) != std::string::npos;
}

// Same bin-membership rule main.mm's -rebuildMediaList already applies:
// `binFilter` empty means "all bins"; a bin id of the literal string used by
// the caller for "no bin" (main.mm uses "__root__"/"__all__" sentinels at
// the AppKit layer) is translated by the caller into `wantRoot`/`Ulid{}`
// before reaching here, so this stays a plain data-model predicate with no
// AppKit-specific sentinel strings baked in.
inline bool MatchesBin(const Ulid& itemBinId, bool anyBin, bool wantRoot,
                       const Ulid& wantBinId) {
    if (anyBin) return true;
    if (wantRoot) return itemBinId.empty();
    return itemBinId == wantBinId;
}

}  // namespace detail

// ---- Audio tab -----------------------------------------------------------
//
// Ingest.cc's ProbeMediaMetadata rejects any file without a video stream
// (see its "no video stream" failure) -- this codebase's library has no
// video-less media entry today, and adding one is new engine capability
// outside this UI ticket's scope (ROADMAP.md F2.3: "this is a UI ticket").
// "Audio-only sources" is therefore read here as "library media whose
// LibraryMedia::has_audio is true" -- the subset of the existing library a
// user would drag onto an audio track -- rather than a file lacking video.
inline bool CarriesAudio(const LibraryMedia& media) { return media.has_audio; }

// Filters `library` down to audio-capable entries within one bin (or all
// bins) and matching a case-insensitive substring `search` against
// filename/codec/path, mirroring the Media tab's existing filter (see
// main.mm's -rebuildMediaList) so the two tabs behave identically. Returns
// pointers into `library`; the caller must keep `library` alive.
inline std::vector<const LibraryMedia*> FilterAudioSources(
    const std::vector<LibraryMedia>& library, bool anyBin, bool wantRoot,
    const Ulid& wantBinId, const std::string& search) {
    const std::string searchLower = detail::Lower(search);
    std::vector<const LibraryMedia*> result;
    for (const LibraryMedia& media : library) {
        if (!CarriesAudio(media)) continue;
        if (!detail::MatchesBin(media.bin_id, anyBin, wantRoot, wantBinId))
            continue;
        if (!searchLower.empty() &&
            !detail::ContainsInsensitive(media.filename, searchLower) &&
            !detail::ContainsInsensitive(media.codec, searchLower) &&
            !detail::ContainsInsensitive(media.path, searchLower))
            continue;
        result.push_back(&media);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const LibraryMedia* left, const LibraryMedia* right) {
                         return left->filename < right->filename;
                     });
    return result;
}

// ---- Captions tab ---------------------------------------------------------

// A short, human-readable one-line description of a caption style's visual
// properties (CaptionStyle has no display `name` of its own -- see
// Document.h). Deterministic and locale-independent (ASCII "pt"/"x" units)
// so it round-trips identically across runs, which the unit test checks.
inline std::string DescribeCaptionStyle(const CaptionStyle& style) {
    return style.font_family + " " + std::to_string(style.font_size) + "pt · " +
           style.position + " · " + style.color;
}

struct CaptionStyleSummary {
    Ulid style_id;
    std::string description;
    // Clips across the whole sequence currently sharing this style as their
    // caption_group_id -- i.e. the size of the caption "run"/group this
    // style names. See Document.h's CaptionStyle comment: a caption group
    // IS a caption_styles entry, clips join it by setting
    // DocumentClip::caption_group_id to the style's id.
    int32_t clip_count = 0;
};

// One entry per sequence.caption_styles, in that vector's existing order
// (append order -- the same order AddCaptionStyleOperation/
// RemoveCaptionStyleOperation already maintain), each annotated with how
// many clips currently belong to that caption group.
inline std::vector<CaptionStyleSummary> SummarizeCaptionStyles(
    const DocumentSequence& sequence) {
    std::vector<CaptionStyleSummary> result;
    result.reserve(sequence.caption_styles.size());
    for (const CaptionStyle& style : sequence.caption_styles) {
        CaptionStyleSummary summary;
        summary.style_id = style.id;
        summary.description = DescribeCaptionStyle(style);
        for (const DocumentTrack& track : sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (clip.caption_group_id == style.id) ++summary.clip_count;
        result.push_back(std::move(summary));
    }
    return result;
}

// Builds the exact SetClipCaptionOperation that joins `clipId` into
// `styleId`'s caption group, keeping whatever caption_text the caller
// already has for that clip (an empty string starts a fresh, untexted
// caption on that clip). The caller applies this via
// EditLog::Apply(document, Operation{op}, ...) -- this function never
// touches a Document.
//
// Named without an "Operation" suffix on purpose: tests/architecture_tests.py
// treats any bare `[A-Z]...Operation` token appearing in main.mm as a claim
// that a struct by that name is registered in Operations.h's variant, and
// this is a factory *function* returning an already-registered struct
// (SetClipCaptionOperation), not a new struct of its own.
inline SetClipCaptionOperation JoinClipToCaptionStyle(
    const Ulid& clipId, const Ulid& styleId, const std::string& captionText) {
    SetClipCaptionOperation operation;
    operation.clip_id = clipId;
    operation.caption_group_id = styleId;
    operation.caption_text = captionText;
    return operation;
}

// Builds the exact SetClipCaptionOperation that removes `clipId` from
// whatever caption group it belongs to (Document.h: an empty
// caption_group_id means "no caption"). See the naming note above.
inline SetClipCaptionOperation ClearClipCaption(const Ulid& clipId) {
    SetClipCaptionOperation operation;
    operation.clip_id = clipId;
    return operation;
}

}  // namespace ui::media_panel

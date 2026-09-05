#pragma once

#include "Document.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// QC-2026-09 (A6) -- a model cannot judge a cut one isolated read_frame at a
// time.  These plans make the editorial sampling deterministic and keep all
// time arithmetic in the engine; rendering is a read-only consequence.
enum class TimelineSheetKind { Contact, Cuts };

struct TimelineSheetSettings {
    // Bounded because one MCP image must stay useful to a vision model.  When
    // there are more candidates, the engine samples the whole timeline at
    // even intervals instead of returning only its beginning.
    int32_t maximum_images = 24;
    int32_t columns = 4;
    int32_t cell_dimension = 320;
    int32_t jpeg_quality = 6;
    std::string ffmpeg_path = "ffmpeg";
};

struct TimelineSheetFrame {
    Ulid clip_id;
    Ulid source_id;
    RationalTime timeline_position;
    RationalTime source_position;
    // "middle", "before" or "after".  A cut pair shares cut_position.
    std::string role;
    RationalTime cut_position{0, 1};
    bool has_cut_position = false;
};

struct TimelineSheetPlan {
    TimelineSheetKind kind = TimelineSheetKind::Contact;
    int64_t total_candidates = 0;
    std::vector<TimelineSheetFrame> frames;
};

// Contact sheets sample the middle frame of visible shots. Cut sheets sample
// the last frame before and first frame after every visible hard cut. Higher
// visible video tracks occlude lower ones exactly as timeline_stats does.
bool BuildTimelineSheetPlan(const Document& document, TimelineSheetKind kind,
                            const TimelineSheetSettings& settings,
                            TimelineSheetPlan& output, std::string& error);

// Compact canonical metadata accompanies the JPEG over CLI and MCP so every
// cell remains addressable by ULID and exact RationalTime.
std::string SerializeTimelineSheetPlan(const TimelineSheetPlan& plan);

// Renders a single JPEG grid. Relative media paths are resolved against
// projectDirectory. The document's display colour transform is applied after
// composition, so log rushes are never handed to the caller untransformed.
bool RenderTimelineSheet(const Document& document,
                         const std::filesystem::path& projectDirectory,
                         const TimelineSheetPlan& plan,
                         const TimelineSheetSettings& settings,
                         std::string& jpegBytes, std::string& error);

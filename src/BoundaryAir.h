#pragma once

#include "Document.h"
#include "Operations.h"
#include "SpeechOnset.h"

#include <cstdint>
#include <string>
#include <vector>

// B4 -- ROADMAP.md. Boundary silence is measured from B1's persisted speech
// groups, never inferred from word timestamps. The defaults preserve three
// source frames around an attack/decay and ignore a boundary shorter than an
// ordinary breath.
struct BoundaryAirSettings {
    int64_t keep_frames = 3;
    int64_t minimum_air_milliseconds = 300;
    SpeechOnsetThresholds speech_thresholds;
};

struct BoundaryAirReport {
    RationalTime head_air{0, 1};
    RationalTime tail_air{0, 1};
    RationalTime head_removed{0, 1};
    RationalTime tail_removed{0, 1};
};

struct JunctionAirReport {
    RationalTime left_tail_air{0, 1};
    RationalTime right_head_air{0, 1};
    RationalTime combined_air{0, 1};
    RationalTime left_removed{0, 1};
    RationalTime right_removed{0, 1};
};

// Resolves both boundaries of one clip into one atomic operation. A boundary
// under minimum_air_milliseconds, or already no longer than keep_frames,
// remains represented in the report but produces no trim.
bool ResolveBoundaryAir(const DocumentClip& clip,
                        const SpeechOnsetReport& envelope,
                        const BoundaryAirSettings& settings,
                        const MediaRate& sourceRate,
                        const std::vector<Ulid>& syncTrackIds,
                        TrimBoundaryAirOperation& operation,
                        BoundaryAirReport& report, std::string& error);

// Resolves the outgoing tail and incoming head together. The minimum applies
// to their sum, because that is the silence the listener hears across the
// splice. Both deltas are emitted together, which makes a second call a no-op
// instead of moving the junction back and forth over successive passes.
bool ResolveJunctionAir(
    const DocumentClip& left, const SpeechOnsetReport& leftEnvelope,
    const MediaRate& leftSourceRate, const DocumentClip& right,
    const SpeechOnsetReport& rightEnvelope, const MediaRate& rightSourceRate,
    const BoundaryAirSettings& settings, const std::vector<Ulid>& syncTrackIds,
    TrimBoundaryAirOperation& operation, JunctionAirReport& report,
    std::string& error);

// Editing defaults shared by CLI and MCP: every member of the anchor's link
// group follows, and every other sync-locked track ripples by the same total.
std::vector<Ulid> LinkedBoundaryClipIds(const Document& document,
                                        const DocumentClip& clip);
std::vector<Ulid> BoundarySyncTrackIds(
    const Document& document, const std::vector<BoundaryAirTrim>& trims);

std::string SerializeBoundaryAirReport(const BoundaryAirReport& report);
std::string SerializeJunctionAirReport(const JunctionAirReport& report);

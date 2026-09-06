#pragma once

// PERF-2026-09. Whether the Record monitor may composite what the frame cache
// holds right now, or has to keep showing its previous image for one more
// tick.
//
// Renderer.mm composites the resolved video layers bottom-to-top and skips a
// null frame, so a layer whose decoder has not caught up does not draw as
// "not ready yet" -- it draws as *nothing*, and whatever sits underneath
// shows through: the track below, or black. Playback crossing a cut onto a
// source the cache has never held is exactly that situation, and it is where
// the flash of the lower track at a cut came from.
//
// This is the presentation *policy*, kept as plain C++ with no AppKit
// dependency the same way UiTheme.h and PanelLayout.h are; main.mm only wires
// it (presentNearestFrameAtDeadline). See tests/playback_presentation_tests.cc.

#include <vector>

namespace playback {

// One resolved video layer at presentation time.
struct LayerReadiness {
    // The edit puts an image in this layer at the current position. An
    // inactive layer is a hole the edit asked for, and reveals lower tracks
    // on purpose.
    bool active = false;
    // The frame cache handed back an image for it -- the requested frame, or
    // during a seek the nearest one it holds. Either way this layer has
    // something to draw.
    bool decoded = false;
    // Its source has a running decoder, so a frame is on its way. False for
    // an offline source, whose layer will never be filled and therefore must
    // not hold the monitor hostage.
    bool decodable = false;
};

// Upper bound on how long the previous image may be held, in display ticks --
// main.mm's timer runs at 60 Hz, so half a second. A decoder that never
// delivers (a file that opens but whose frames all fail to decode) would
// otherwise freeze the monitor for good; past this budget the composite is
// presented as it stands, hole included, which is at least honest about the
// state the player is in.
inline constexpr int kMaximumHeldTicks = 30;

struct HoldDecision {
    bool hold = false;
    int held_ticks = 0;
};

// `programPresented` says the monitor has already drawn at least once, so
// there is a previous image worth keeping. `heldTicks` is the counter this
// function returned on the previous tick.
inline HoldDecision DecideProgramHold(const std::vector<LayerReadiness>& layers,
                                      bool programPresented, int heldTicks) {
    bool late = false;
    for (const LayerReadiness& layer : layers) {
        if (layer.active && !layer.decoded && layer.decodable) late = true;
    }
    if (!late) return {false, 0};
    const bool hold = programPresented && heldTicks < kMaximumHeldTicks;
    // The counter resets only once nothing is late any more: spending the
    // budget has to end the hold, not restart it one tick later and alternate
    // between a held image and a hole.
    return {hold, hold ? heldTicks + 1 : heldTicks};
}

}  // namespace playback

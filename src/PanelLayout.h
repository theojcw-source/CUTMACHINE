#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

// F2.1 -- ROADMAP.md. This header is the entire layout policy for the
// panels F2.2 (Inspector), F2.3 (Media), F2.4 (Chat) and F2.5 (playback
// transport) build on top of.
//
// PHILOSOPHY.md's amended non-buts allow remembering interface state (which
// theme, which tab was last open, whether a panel is shown) as a *local*
// preference, never inside the project document -- and call out movable,
// user-rearrangeable panels specifically as the kind of thing that used to
// be banned outright and is now merely *not required*. Given F2.1 sets the
// precedent every later UI ticket follows, the layout below is fixed: one
// slot per future panel, one dock per slot, one position within that dock.
// There is no add/remove/reorder API and nothing here is ever serialized --
// it is a compiled-in table, not data a user (or an agent) can rearrange.
// The only thing a local preference may record is *which already-fixed tab
// is currently selected* (see LastActiveTabPreferenceKey below), and that
// preference lives in NSUserDefaults (UiPreferences.h), never in
// Document/ProjectStorage.
namespace ui {

enum class PanelSlot { Inspector, Media, Chat, Transport };

enum class PanelDock { Left, Right, Bottom };

struct PanelSlotDescriptor {
    PanelSlot slot;
    const char* identifier;  // stable, ASCII; used only for preference keys
    const char* title;       // display title (matches the app's French UI)
    PanelDock dock;
    int order;  // position within `dock`, ascending; unique per dock
};

// The one and only panel layout, in declaration order. F2.3's media
// browser already has dedicated screen real estate (main.mm's existing
// left-hand mediaPanel, built before this ticket); it is listed here so the
// registry and its local-preference keys stay complete and every panel
// slot -- present or future -- is described in exactly one place, even
// though F2.3 does not need CMPanelHostView's tab chrome to be reachable.
inline const std::array<PanelSlotDescriptor, 4>& FixedPanelLayout() {
    static const std::array<PanelSlotDescriptor, 4> kLayout{{
        {PanelSlot::Media, "media", "Médiathèque", PanelDock::Left, 0},
        {PanelSlot::Inspector, "inspector", "Inspecteur", PanelDock::Right, 0},
        {PanelSlot::Chat, "chat", "Agent", PanelDock::Right, 1},
        {PanelSlot::Transport, "transport", "Lecture", PanelDock::Bottom, 0},
    }};
    return kLayout;
}

inline const PanelSlotDescriptor* FindPanelSlot(PanelSlot slot) {
    for (const PanelSlotDescriptor& descriptor : FixedPanelLayout())
        if (descriptor.slot == slot) return &descriptor;
    return nullptr;
}

inline const PanelSlotDescriptor* FindPanelSlotByIdentifier(
    const std::string& identifier) {
    for (const PanelSlotDescriptor& descriptor : FixedPanelLayout())
        if (identifier == descriptor.identifier) return &descriptor;
    return nullptr;
}

// The slots assigned to `dock`, sorted by `order`. This is the fixed
// tab/stack order a dock's host view builds once at construction time and
// never mutates afterward.
inline std::vector<const PanelSlotDescriptor*> PanelSlotsInDock(
    PanelDock dock) {
    std::vector<const PanelSlotDescriptor*> result;
    for (const PanelSlotDescriptor& descriptor : FixedPanelLayout())
        if (descriptor.dock == dock) result.push_back(&descriptor);
    std::sort(result.begin(), result.end(),
              [](const PanelSlotDescriptor* a, const PanelSlotDescriptor* b) {
                  return a->order < b->order;
              });
    return result;
}

// The NSUserDefaults key CMUiPreferences (UiPreferences.h, AppKit-only)
// uses to remember which tab was last active in a given dock. Purely a
// per-machine local preference -- see the file-level comment above. Kept
// next to the slot table so the key format has exactly one definition.
inline std::string LastActiveTabPreferenceKey(PanelDock dock) {
    switch (dock) {
        case PanelDock::Left:
            return "ui.dock.left.lastActiveTab";
        case PanelDock::Right:
            return "ui.dock.right.lastActiveTab";
        case PanelDock::Bottom:
            return "ui.dock.bottom.lastActiveTab";
    }
    return "ui.dock.unknown.lastActiveTab";
}

}  // namespace ui

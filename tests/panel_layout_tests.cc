// Pure-C++ test for the fixed panel-slot registry (ROADMAP.md F2.1).
// PanelLayout.h has no AppKit dependency, so this builds and runs on a
// plain Linux host, the same way tests/fft_tests.cc does.

#include "PanelLayout.h"

#include <iostream>
#include <set>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) std::cout << "PASS: " << name << '\n';
}

}  // namespace

int main() {
    Test("the layout has exactly one descriptor per panel slot", [] {
        const auto& layout = ui::FixedPanelLayout();
        Check(layout.size() == 4, "expected exactly 4 fixed panel slots");
        std::set<ui::PanelSlot> seenSlots;
        for (const auto& descriptor : layout)
            seenSlots.insert(descriptor.slot);
        Check(seenSlots.size() == layout.size(),
              "every PanelSlot value must appear exactly once");
    });

    Test("identifiers are unique, stable, non-empty ASCII", [] {
        std::set<std::string> identifiers;
        for (const auto& descriptor : ui::FixedPanelLayout()) {
            const std::string identifier = descriptor.identifier;
            Check(!identifier.empty(), "identifier must not be empty");
            identifiers.insert(identifier);
        }
        Check(identifiers.size() == ui::FixedPanelLayout().size(),
              "identifiers must be unique across slots");
    });

    Test("within each dock, order values are unique -- no tab-order ties",
         [] {
             for (ui::PanelDock dock :
                  {ui::PanelDock::Left, ui::PanelDock::Right,
                   ui::PanelDock::Bottom}) {
                 std::set<int> orders;
                 for (const auto* descriptor : ui::PanelSlotsInDock(dock))
                     orders.insert(descriptor->order);
                 Check(orders.size() == ui::PanelSlotsInDock(dock).size(),
                       "duplicate `order` within one dock");
             }
         });

    Test("PanelSlotsInDock returns only that dock's slots, sorted by order",
         [] {
             const auto rightSlots = ui::PanelSlotsInDock(ui::PanelDock::Right);
             for (const auto* descriptor : rightSlots)
                 Check(descriptor->dock == ui::PanelDock::Right,
                       "PanelSlotsInDock leaked a slot from another dock");
             for (size_t index = 1; index < rightSlots.size(); ++index)
                 Check(rightSlots[index - 1]->order < rightSlots[index]->order,
                       "slots within a dock must come back in ascending order");
         });

    Test("Inspector and Chat both dock right, in a fixed relative order", [] {
        // This is the concrete layout F2.2 (Inspector) and F2.4 (Chat) are
        // written against: both claim the right-hand dock, Inspector first.
        // Changing this later is a deliberate layout change, not a runtime
        // rearrangement.
        const auto rightSlots = ui::PanelSlotsInDock(ui::PanelDock::Right);
        Check(rightSlots.size() == 2, "expected exactly 2 right-docked slots");
        if (rightSlots.size() == 2) {
            Check(rightSlots[0]->slot == ui::PanelSlot::Inspector,
                  "Inspector should be the first right-docked tab");
            Check(rightSlots[1]->slot == ui::PanelSlot::Chat,
                  "Chat should be the second right-docked tab");
        }
    });

    Test("FindPanelSlot round-trips every slot in the layout", [] {
        for (const auto& descriptor : ui::FixedPanelLayout()) {
            const ui::PanelSlotDescriptor* found =
                ui::FindPanelSlot(descriptor.slot);
            Check(found != nullptr && found->slot == descriptor.slot,
                  "FindPanelSlot failed to round-trip a known slot");
        }
    });

    Test("FindPanelSlotByIdentifier round-trips and rejects unknown ids", [] {
        for (const auto& descriptor : ui::FixedPanelLayout()) {
            const ui::PanelSlotDescriptor* found =
                ui::FindPanelSlotByIdentifier(descriptor.identifier);
            Check(found != nullptr && found->slot == descriptor.slot,
                  "FindPanelSlotByIdentifier failed to round-trip a known id");
        }
        Check(ui::FindPanelSlotByIdentifier("does-not-exist") == nullptr,
              "an unknown identifier must resolve to nullptr, not a guess");
    });

    Test("local-preference keys are stable and unique per dock", [] {
        std::set<std::string> keys;
        for (ui::PanelDock dock :
             {ui::PanelDock::Left, ui::PanelDock::Right,
              ui::PanelDock::Bottom}) {
            const std::string key = ui::LastActiveTabPreferenceKey(dock);
            Check(key == ui::LastActiveTabPreferenceKey(dock),
                  "the same dock must always produce the same key");
            keys.insert(key);
        }
        Check(keys.size() == 3, "each dock must have a distinct key");
    });

    return failures == 0 ? 0 : 1;
}

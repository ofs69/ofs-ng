#pragma once

#include "Core/CustomCommand.h"
#include <string>

namespace ofs {

// ── Custom-command intents ────────────────────────────────────────────────────────────────────────
// UI editor → CustomCommandStore. The Shortcut window never calls the store directly (UI pushes
// events only); it pushes one of these and the store reconciles its definition list + file.
//
// RemoveCustomCommandEvent has *two* independent subscribers on the one user intent: CustomCommandStore
// (drops the definition + saves) and BindingSystem (prunes any bindings to that id + saves). Two
// handlers, one event — so no service calls another.

// Add a new custom command. The store allocates its stable id (`def.id` is ignored on add).
struct AddCustomCommandEvent {
    CustomCommand def;
};

// Update an existing custom command in place. `def.id` identifies the target; editing params or name
// keeps the id so any existing bindings stay attached.
struct UpdateCustomCommandEvent {
    CustomCommand def;
};

// Delete a custom command by id. Custom ids are append-only and never reused, so a stale binding has
// no value — hence BindingSystem prunes on this event (provider/plugin removals stay tolerated).
struct RemoveCustomCommandEvent {
    std::string id;
};

} // namespace ofs

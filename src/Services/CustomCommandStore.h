#pragma once

#include "Core/CustomCommand.h"
#include <vector>

namespace ofs {

class EventQueue;
class CommandRegistry;
class CustomCommandTemplateRegistry;
struct AddCustomCommandEvent;
struct UpdateCustomCommandEvent;
struct RemoveCustomCommandEvent;

// Owns the user's custom-command definitions and their custom_commands.json file, and is their command
// *provider* — the persistent, bindable analogue of CommandProviders. Each definition is built, edited,
// and (de)serialized by the CustomCommandTemplate its `templateKey` resolves to, so this store knows
// nothing about individual kinds — adding one is registering a template, not touching this class.
// Definitions register into the CommandRegistry as source=Custom. Mutations arrive only as events (the UI
// never calls this directly).
//
// Binding cleanup on delete is NOT this service's job: BindingSystem subscribes to the same
// RemoveCustomCommandEvent and prunes its own bindings — two independent handlers on one intent, so no
// service calls another.
class CustomCommandStore {
  public:
    CustomCommandStore(EventQueue &eq, CommandRegistry &registry, const CustomCommandTemplateRegistry &templates);

    // Parse custom_commands.json and register each definition. Call once at init, AFTER native commands
    // are registered and BEFORE BindingSystem::loadBindings(), so a binding to custom.<n> resolves on
    // first boot. Missing/unparseable file ⇒ empty set.
    void load();

    [[nodiscard]] const std::vector<CustomCommand> &commands() const { return commands_; }

  private:
    void onAdd(const AddCustomCommandEvent &e);
    void onUpdate(const UpdateCustomCommandEvent &e);
    void onRemove(const RemoveCustomCommandEvent &e);

    // removeBySource(Custom) + re-add all. The set is tiny, so a full rebuild beats surgical edits and
    // keeps registration order deterministic.
    void reregisterAll();
    void save() const;

    EventQueue &eventQueue_;
    CommandRegistry &commandRegistry_;
    const CustomCommandTemplateRegistry &templates_;

    std::vector<CustomCommand> commands_;
    int nextId_ = 0; // monotonic; a new def takes "custom.<nextId_++>". Never decreases, never reused.
};

} // namespace ofs

#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace ofs {

// One user-defined command. A plain serializable struct with no behavior — its CustomCommandTemplate
// (resolved by `templateKey`, see Services/CustomCommandTemplate.h) renders its editor, turns it into a
// Command, and summarizes it. `params` is an opaque per-template bag: the host never interprets it, only
// the resolved template reads the keys it owns. Keeping params generic decouples this struct from the kind
// set: a new in-tree kind adds its own keys without touching this struct, and (de)serialization needs no
// per-kind hook — the bag *is* the wire format.
struct CustomCommand {
    std::string id;          // stable: "custom.<n>" — assigned once, never changes on edit/rename
    std::string name;        // user title (NOT localized); empty ⇒ the template's summary is used as the title
    std::string templateKey; // which template builds/edits/summarizes this; matches CustomCommandTemplate::key

    // Per-template parameters. Object-initialized (not null) so a fresh draft is safe to read with
    // json::value(...), which throws on a non-object. The template owns the key set and their wire shapes.
    nlohmann::json params = nlohmann::json::object();
};

} // namespace ofs

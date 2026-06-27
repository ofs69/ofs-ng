#include "Services/CustomCommandStore.h"

#include "Core/CommandEvents.h"
#include "Core/EventQueue.h"
#include "Services/CommandRegistry.h"
#include "Services/CustomCommandTemplate.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

namespace ofs {

static std::filesystem::path getCustomCommandsPath() {
    return ofs::util::getPrefPath() / "custom_commands.json";
}

// On-disk schema version for custom_commands.json. Bump on an incompatible change; load() refuses
// a file newer than this rather than misreading it.
static constexpr int kCustomCommandsVersion = 1;

// The numeric suffix of a "custom.<n>" id, or -1 if it doesn't fit the pattern (hand-edited / foreign).
static int idNumber(const std::string &id) {
    constexpr std::string_view kPrefix = "custom.";
    if (!id.starts_with(kPrefix))
        return -1;
    int n = 0;
    const char *first = id.data() + kPrefix.size();
    const char *last = id.data() + id.size();
    const auto [ptr, ec] = std::from_chars(first, last, n);
    if (ec != std::errc{} || ptr != last)
        return -1;
    return n;
}

CustomCommandStore::CustomCommandStore(EventQueue &eq, CommandRegistry &registry,
                                       const CustomCommandTemplateRegistry &templates)
    : eventQueue_(eq), commandRegistry_(registry), templates_(templates) {
    eventQueue_.on<AddCustomCommandEvent>([this](const AddCustomCommandEvent &e) { onAdd(e); });
    eventQueue_.on<UpdateCustomCommandEvent>([this](const UpdateCustomCommandEvent &e) { onUpdate(e); });
    eventQueue_.on<RemoveCustomCommandEvent>([this](const RemoveCustomCommandEvent &e) { onRemove(e); });
}

void CustomCommandStore::load() {
    commands_.clear();
    nextId_ = 0;

    auto text = ofs::util::readFile(getCustomCommandsPath());
    if (!text) {
        reregisterAll(); // nothing to register, but clears any stale Custom entries
        return;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(*text);
        if (j.value("version", kCustomCommandsVersion) > kCustomCommandsVersion) {
            OFS_CORE_ERROR("custom_commands.json version {} is newer than supported version {}; ignoring.",
                           j.value("version", 0), kCustomCommandsVersion);
            reregisterAll();
            return;
        }
        nextId_ = j.value("nextId", 0);
        if (j.contains("commands")) {
            for (const auto &entry : j["commands"]) {
                if (!entry.is_object())
                    continue; // hand-edited non-object entry — skip (keeps params a valid object)
                CustomCommand def;
                def.id = entry.value("id", std::string{});
                def.name = entry.value("name", std::string{});
                def.templateKey = entry.value("kind", std::string{});
                if (!templates_.find(def.templateKey))
                    continue; // unknown/absent kind — forward-tolerant skip (no template registered)
                if (def.id.empty())
                    continue;
                // The bag is the wire format: carry every kind-specific key through verbatim. The
                // resolved template reads the keys it owns and ignores the rest, so an unknown value
                // (e.g. a stale granularity) round-trips and only degrades to a default at build —
                // it never drops the command. Strip the structural keys so params holds params only.
                def.params = entry;
                def.params.erase("id");
                def.params.erase("name");
                def.params.erase("kind");
                // De-duplicate a hand-edited duplicate id: last wins.
                std::erase_if(commands_, [&def](const CustomCommand &c) { return c.id == def.id; });
                commands_.push_back(std::move(def));
            }
        }
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("CustomCommandStore: failed to load: {}", e.what());
        commands_.clear();
    }

    // Bump the allocator past any id seen, in case the file was hand-edited with a stale nextId.
    for (const auto &c : commands_)
        nextId_ = std::max(nextId_, idNumber(c.id) + 1);

    reregisterAll();
}

void CustomCommandStore::onAdd(const AddCustomCommandEvent &e) {
    CustomCommand def = e.def;
    def.id = fmt::format("custom.{}", nextId_++); // store assigns the stable id; any incoming id ignored
    commands_.push_back(std::move(def));
    reregisterAll();
    save();
}

void CustomCommandStore::onUpdate(const UpdateCustomCommandEvent &e) {
    auto it = std::ranges::find_if(commands_, [&e](const CustomCommand &c) { return c.id == e.def.id; });
    if (it == commands_.end())
        return;
    const std::string id = it->id; // editing keeps the id so existing bindings stay attached
    *it = e.def;
    it->id = id;
    reregisterAll();
    save();
}

void CustomCommandStore::onRemove(const RemoveCustomCommandEvent &e) {
    const auto removed = std::erase_if(commands_, [&e](const CustomCommand &c) { return c.id == e.id; });
    if (removed == 0)
        return;
    reregisterAll();
    save();
    // Binding cleanup is BindingSystem's own handler for this same event — not called from here.
}

void CustomCommandStore::reregisterAll() {
    commandRegistry_.removeBySource(CommandSource::Custom);
    for (const auto &def : commands_)
        if (const CustomCommandTemplate *t = templates_.find(def.templateKey))
            commandRegistry_.add(t->build(def));
}

void CustomCommandStore::save() const {
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &c : commands_) {
            // Start from the kind-specific param bag, then stamp the structural keys. "kind" is the
            // template's stable id; the template owns everything else inside params.
            nlohmann::json e = c.params;
            e["id"] = c.id;
            e["name"] = c.name;
            e["kind"] = c.templateKey;
            arr.push_back(std::move(e));
        }
        nlohmann::json j;
        j["version"] = kCustomCommandsVersion;
        j["nextId"] = nextId_;
        j["commands"] = std::move(arr);
        ofs::util::writeFile(getCustomCommandsPath(), j.dump(4));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("CustomCommandStore: failed to save: {}", e.what());
    }
}

} // namespace ofs

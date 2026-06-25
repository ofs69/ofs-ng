#pragma once

#include <string>
#include <vector>

namespace ofs {

// Shared implementation for the three interaction-mode registries (edit mode / navigator / selection
// mode). Each is a list of selectable entries seeded with one always-present native entry that owns no
// plugin state and can never be removed, so it is always a safe fallback when a stored or plugin id is
// unknown. The only per-registry differences are the `Entry` payload type and the native entry's id;
// everything else (lookup, in-place republish on reload, per-plugin removal) is identical, so it lives
// here once. `Entry` must expose `std::string id` and `std::string owningPlugin`.
template <class Entry> class ModeRegistry {
  public:
    explicit ModeRegistry(std::string nativeId) { entries_.push_back(Entry{.id = std::move(nativeId)}); }

    [[nodiscard]] bool has(const std::string &id) const { return find(id) != nullptr; }

    // Validity window: the returned pointer points into entries_, which add() and removeByPlugin()
    // mutate — a vector realloc or erase invalidates it. Treat the result as valid only within the
    // current event handler / frame; never cache an Entry* across a plugin load or unload. The routers
    // re-find by id on every dispatch for exactly this reason.
    [[nodiscard]] const Entry *find(const std::string &id) const {
        for (const auto &e : entries_)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    // Publish an entry. A reload re-publishes under the same id, so replace an existing entry in place
    // rather than appending a duplicate.
    void add(Entry entry) {
        for (auto &e : entries_)
            if (e.id == entry.id) {
                e = std::move(entry);
                return;
            }
        entries_.push_back(std::move(entry));
    }

    // Drop every entry owned by `plugin` (on unload). The native entry has no owning plugin, so it
    // always survives — the fallback target can never itself be removed.
    void removeByPlugin(const std::string &plugin) {
        std::erase_if(entries_, [&](const Entry &e) { return e.owningPlugin == plugin; });
    }

    // Same validity window as find(): the span dangles across add()/removeByPlugin(). Read it within
    // the frame; don't retain references to its elements across a plugin load/unload.
    [[nodiscard]] const std::vector<Entry> &entries() const { return entries_; }

  private:
    std::vector<Entry> entries_;
};

} // namespace ofs

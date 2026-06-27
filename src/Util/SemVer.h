#pragma once

#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Minimal semantic-version parser + precedence comparison for the update checker. The running build's
// own version comes from `ofs::generated::kGitTag` (git describe --tags --dirty), so the parser has to
// tolerate two shapes that a plain semver string does not:
//
//   v1.0.0                 a clean release tag (also the shape of every GitHub release tag_name)
//   v1.0.0-5-gabc1234      a DEV build 5 commits past v1.0.0 (git describe's "-<N>-g<hash>" tail)
//   v1.0.0-dirty           uncommitted changes on top of the tag
//   v1.2.0-beta.1          a genuine semver prerelease (sorts BEFORE v1.2.0)
//
// The "-<N>-g<hash>" tail and "-dirty" are git build markers, NOT a semver prerelease: a dev build past
// a tag is *newer* than the tag, whereas a prerelease is *older* than its release. They are parsed into
// `commitsAhead` / `dirty` and kept out of `prerelease` so the precedence below stays correct.

namespace ofs::util {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease; // semver prerelease identifiers, e.g. "beta.1" ("" = stable release)
    int commitsAhead = 0;   // git-describe "-<N>-g<hash>": commits past the tag (dev build); 0 for a clean tag
    bool dirty = false;     // git-describe "-dirty"
};

namespace detail {

// Parse a run of ASCII digits into `out`. Returns false on overflow or if `s` is not all digits.
inline bool parseUInt(std::string_view s, int &out) {
    if (s.empty())
        return false;
    const char *begin = s.data();
    const char *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end && out >= 0;
}

inline bool isAllDigits(std::string_view s) {
    if (s.empty())
        return false;
    for (char c : s)
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            return false;
    return true;
}

// git describe's machine tail is "-<N>-g<hash>" (decimal N, then a literal 'g' + abbreviated hex commit).
// Detect it on the LAST two dash-separated fields so it is never mistaken for a semver prerelease label.
inline bool isDescribeTail(std::string_view countField, std::string_view commitField) {
    if (!isAllDigits(countField))
        return false;
    if (commitField.size() < 2 || commitField[0] != 'g')
        return false;
    for (char c : commitField.substr(1))
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
            return false;
    return true;
}

// Compare two dot-separated semver prerelease strings by §11 precedence. An empty prerelease is a stable
// release and outranks any non-empty one. Per-identifier: all-numeric compare numerically; numeric ranks
// below alphanumeric; alphanumeric compare by ASCII; a shorter run of equal identifiers ranks below a
// longer one. Returns <0 / 0 / >0.
inline int comparePrerelease(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty())
        return 0;
    if (a.empty())
        return 1; // a is a release, b a prerelease -> a is greater
    if (b.empty())
        return -1;

    size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        size_t na = a.find('.', ia);
        size_t nb = b.find('.', ib);
        std::string_view ida = a.substr(ia, na == std::string_view::npos ? std::string_view::npos : na - ia);
        std::string_view idb = b.substr(ib, nb == std::string_view::npos ? std::string_view::npos : nb - ib);

        const bool numA = isAllDigits(ida);
        const bool numB = isAllDigits(idb);
        if (numA && numB) {
            int va = 0, vb = 0;
            parseUInt(ida, va);
            parseUInt(idb, vb);
            if (va != vb)
                return va < vb ? -1 : 1;
        } else if (numA != numB) {
            return numA ? -1 : 1; // numeric identifiers have lower precedence than alphanumeric
        } else if (ida != idb) {
            return ida < idb ? -1 : 1;
        }

        ia = (na == std::string_view::npos) ? a.size() : na + 1;
        ib = (nb == std::string_view::npos) ? b.size() : nb + 1;
    }
    const bool aMore = ia < a.size();
    const bool bMore = ib < b.size();
    if (aMore == bMore)
        return 0;
    return aMore ? 1 : -1; // a larger set of pre-release fields has higher precedence
}

} // namespace detail

// Parse a version string. Accepts an optional leading 'v'/'V', a mandatory MAJOR.MINOR.PATCH core, and an
// optional "-<rest>" tail in which a trailing git-describe "-<N>-g<hash>" and "-dirty" are stripped into
// `commitsAhead`/`dirty` and the remainder kept as `prerelease`. Returns nullopt if the core is missing
// or malformed.
inline std::optional<SemVer> parseSemVer(std::string_view s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
        s.remove_prefix(1);

    const size_t dash = s.find('-');
    std::string_view core = (dash == std::string_view::npos) ? s : s.substr(0, dash);

    const size_t d1 = core.find('.');
    if (d1 == std::string_view::npos)
        return std::nullopt;
    const size_t d2 = core.find('.', d1 + 1);
    if (d2 == std::string_view::npos)
        return std::nullopt;

    SemVer v;
    if (!detail::parseUInt(core.substr(0, d1), v.major))
        return std::nullopt;
    if (!detail::parseUInt(core.substr(d1 + 1, d2 - d1 - 1), v.minor))
        return std::nullopt;
    if (!detail::parseUInt(core.substr(d2 + 1), v.patch))
        return std::nullopt;

    if (dash == std::string_view::npos)
        return v;

    // Split the tail on '.'-preserving dashes into fields, then peel git markers off the end.
    std::string tail(s.substr(dash + 1));
    std::vector<std::string_view> fields;
    for (size_t pos = 0;;) {
        size_t next = tail.find('-', pos);
        fields.emplace_back(
            std::string_view(tail).substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }

    if (!fields.empty() && fields.back() == "dirty") {
        v.dirty = true;
        fields.pop_back();
    }
    if (fields.size() >= 2 && detail::isDescribeTail(fields[fields.size() - 2], fields.back())) {
        detail::parseUInt(fields[fields.size() - 2], v.commitsAhead);
        fields.pop_back();
        fields.pop_back();
    }

    // Whatever remains is a genuine semver prerelease (its own dot-separated identifiers were never split,
    // because we only split on '-'). Rejoin the surviving fields with '-' to reconstruct it verbatim.
    std::string pre;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i)
            pre += '-';
        pre.append(fields[i]);
    }
    v.prerelease = std::move(pre);
    return v;
}

// Total order by semver precedence, with the git dev markers as final tiebreakers so a dev build past a
// tag outranks the clean tag (and a dirty tree outranks a clean one at the same commit). Returns <0/0/>0.
inline int compareSemVer(const SemVer &a, const SemVer &b) {
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    if (int pr = detail::comparePrerelease(a.prerelease, b.prerelease); pr != 0)
        return pr;
    if (a.commitsAhead != b.commitsAhead)
        return a.commitsAhead < b.commitsAhead ? -1 : 1;
    if (a.dirty != b.dirty)
        return a.dirty ? 1 : -1;
    return 0;
}

// True when `latest` is strictly newer than `current` — the gate for offering an update.
inline bool isNewer(const SemVer &latest, const SemVer &current) {
    return compareSemVer(latest, current) > 0;
}

} // namespace ofs::util

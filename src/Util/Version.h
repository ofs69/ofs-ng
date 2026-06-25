#pragma once

#include <string>

namespace ofs {

// The canonical build-identity string: "ofs-ng <tag> [<shortHash>] [DEBUG]". Each segment is omitted
// when its source is empty — a tag-less history drops the tag, a non-git build drops the hash — and the
// [DEBUG] marker appears only in debug builds. Built once (the git identity is fixed at compile time)
// and shared by the OS window title and the welcome-screen header so the two can never drift apart.
const std::string &versionTitle();

} // namespace ofs

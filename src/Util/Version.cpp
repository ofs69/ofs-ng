#include "Util/Version.h"

#include <OfsBuildInfo.h> // generated: git identity baked into the binary
#include <spdlog/fmt/fmt.h>

namespace ofs {

const std::string &versionTitle() {
    // Static: the git identity is fixed at compile time, so this is built once and reused every frame
    // (the welcome screen reads it per-frame) with no allocation churn.
    //
    // Built with plain statements in an IIFE rather than one fmt::format whose arguments are ternaries:
    // those conditional std::string temporaries are materialized into the static-init full-expression,
    // where clang (libstdc++) emits their lifetime.end before the destructor runs. ASan flags it as
    // stack-use-after-scope and it crashed the headless Linux build (freeing an out-of-scope temporary).
    // Statement scoping keeps each temporary's lifetime unambiguous.
    static const std::string title = [] {
        std::string s = "ofs-ng";
        if (!generated::kGitTag.empty())
            s += fmt::format(" {}", generated::kGitTag);
        if (!generated::kGitCommitShort.empty())
            s += fmt::format(" [{}]", generated::kGitCommitShort);
#ifndef NDEBUG
        s += " [DEBUG]";
#endif
        return s;
    }();
    return title;
}

} // namespace ofs

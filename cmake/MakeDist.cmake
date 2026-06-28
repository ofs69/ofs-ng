# Packages this build's runtime artifacts into dist/<app>-<platform>-<config>-<tag>+<shorthash>.zip, and
# (on Windows only — the sole toolchain we ship standalone symbol files for) the debug symbols into a
# sibling dist/…-pdb.zip.
#
# Driven by the `dist` custom target (see src/CMakeLists.txt) after the app is built. The git tag and
# short commit hash are read here, at build time rather than configure time, so the archive name tracks
# the checked-out revision without forcing a reconfigure. Symbols are split into the -pdb archive so a
# published release stays lean while its crash dumps can still be symbolicated from the symbol zip.
#
# Artifacts are named explicitly, NOT copied from bin/ wholesale: bin/<config> is shared by both AVX
# variants (ofs-ng and ofs-ng-no-avx write into the same dir), so a blanket copy sweeps in the other
# variant's exe/pdb. localization_gen (a build-time host generator) is simply never listed.
#
# Platform: the file *set* is OS-specific — the binary extension, whether libmpv/SDL/nethost ship
# alongside or come from the system, and whether a separate symbol file even exists all differ. The
# binary name and the SDL shared-lib path are therefore passed in as resolved genexes (APP_FILE,
# SDL_SHARED_FILE) rather than reconstructed here, and the loose runtime DLLs are gated on SYSTEM_NAME.
# Windows is the shipping target and is fully handled; on other platforms the archive is best-effort
# (correct binary + data.pak + SDL if shared) and logs what it does not bundle (system libmpv, DWARF/
# dSYM symbols), rather than silently emitting a Windows-shaped, broken zip.
#
# Args (passed via -D on the `cmake -P` command line):
#   BIN_DIR         directory holding the built app and its staged runtime files
#   DIST_DIR        output directory for the .zip (created if absent)
#   APP_NAME        archive base name and symbol-file stem (the app's OUTPUT_NAME, e.g. ofs-ng-no-avx)
#   APP_FILE        exact binary filename incl. platform extension (ofs-ng.exe / ofs-ng-no-avx)
#   CONFIG          build configuration ($<CONFIG>, e.g. Release/Debug) — folded into the archive name
#   SYSTEM_NAME     CMAKE_SYSTEM_NAME (Windows / Linux / Darwin) — gates the loose runtime DLLs
#   AVX             OFS_AVX value (ON/OFF) — OFF guarantees a "-no-avx" marker in the archive name
#   SDL_SHARED_FILE resolved path to the SDL3 shared lib when SDL is linked dynamically; empty if static
#   GIT_EXECUTABLE  path to git; empty/unset → tag and hash blank (built outside a checkout / no git)
#   WORK_DIR        repository working directory to query

if (NOT DEFINED BIN_DIR OR NOT DEFINED DIST_DIR OR NOT DEFINED APP_NAME OR NOT DEFINED CONFIG)
    message(FATAL_ERROR "MakeDist: BIN_DIR, DIST_DIR, APP_NAME and CONFIG are required")
endif ()

set(_is_windows FALSE)
if (SYSTEM_NAME STREQUAL "Windows")
    set(_is_windows TRUE)
endif ()
# APP_FILE carries the platform-correct extension; fall back to the bare name for an older caller.
if (NOT DEFINED APP_FILE OR APP_FILE STREQUAL "")
    set(APP_FILE "${APP_NAME}")
endif ()

set(_tag "")
set(_short "")
# ERROR_QUIET + no result check: a missing git, a non-repo dir, or a tag-less history each leave the
# corresponding field empty (matching cmake/GenBuildInfo.cmake), so the archive still gets a name.
if (DEFINED GIT_EXECUTABLE AND NOT GIT_EXECUTABLE STREQUAL "")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WORK_DIR}" describe --tags --dirty
            OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WORK_DIR}" rev-parse --short HEAD
            OUTPUT_VARIABLE _short OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif ()

# A non-AVX build renames its OUTPUT_NAME to "…-no-avx", so APP_NAME usually already carries the marker;
# add it explicitly only when AVX is OFF and it is somehow absent, so the variant is always legible in
# the archive name without ever doubling the token.
set(_variant "${APP_NAME}")
if (DEFINED AVX AND NOT AVX AND NOT _variant MATCHES "no-avx")
    set(_variant "${_variant}-no-avx")
endif ()

# Short, stable platform tag so the OS a build targets is legible at a glance (win/linux/mac). Unknown
# systems fall back to the lowercased CMAKE_SYSTEM_NAME rather than dropping the tag.
if (_is_windows)
    set(_plat "win")
elseif (SYSTEM_NAME STREQUAL "Darwin")
    set(_plat "mac")
elseif (SYSTEM_NAME STREQUAL "Linux")
    set(_plat "linux")
else ()
    string(TOLOWER "${SYSTEM_NAME}" _plat)
endif ()

# <variant>-<plat>-<config>-<tag>+<shorthash>: the hash rides as semver-style build metadata. It must
# NOT be bracketed — the gh CLI glob-expands its asset arguments itself (so it works on a brackets-are-
# literal Windows shell), and CMake file(GLOB) treats them the same, so "[<hash>]" silently matches
# nothing in both. A tag-less history yields …-g<hash> (git-describe style); no git at all yields a bare
# …-<config>, still a valid (if unhelpful) name rather than a failure.
string(TOLOWER "${CONFIG}" _config_lc)
set(_name "${_variant}-${_plat}-${_config_lc}")
if (NOT _tag STREQUAL "" AND NOT _short STREQUAL "")
    string(APPEND _name "-${_tag}+${_short}")
elseif (NOT _tag STREQUAL "")
    string(APPEND _name "-${_tag}")
elseif (NOT _short STREQUAL "")
    string(APPEND _name "-g${_short}")
endif ()
set(_stage "${DIST_DIR}/${_name}")
set(_zip "${DIST_DIR}/${_name}.zip")
# Symbols travel in a sibling zip whose root folder is suffixed -pdb, so the two unpack side by side.
set(_sym_name "${_name}-pdb")
set(_sym "${DIST_DIR}/${_sym_name}")
set(_sym_zip "${DIST_DIR}/${_sym_name}.zip")

file(MAKE_DIRECTORY "${DIST_DIR}")
file(REMOVE_RECURSE "${_stage}" "${_sym}")
file(REMOVE "${_zip}" "${_sym_zip}")
file(MAKE_DIRECTORY "${_stage}")
# The symbol stage only exists on Windows; non-Windows toolchains ship no separate symbol zip.
if (_is_windows)
    file(MAKE_DIRECTORY "${_sym}")
endif ()

# App binary (exact name incl. extension) and the always-present asset pak.
set(_want
        "${APP_FILE}"
        "data.pak")
if (_is_windows)
    # Windows bundles libmpv next to the binary (auto-downloaded at build; src/CMakeLists.txt). Other
    # platforms link the host's system libmpv, so there is nothing to stage there.
    list(APPEND _want "libmpv-2.dll")
    # The ffmpeg/ffprobe CLIs ship alongside on Windows (auto-downloaded at build; src/CMakeLists.txt).
    # Other platforms expect them on PATH from the system package manager.
    list(APPEND _want "ffmpeg.exe" "ffprobe.exe")
    # nethost.dll ships only in Debug — Release statically links libnethost (/MT). Outside Debug it is
    # not produced, so the EXISTS guard below would skip a stale one anyway; gate it to be explicit.
    if (CONFIG STREQUAL "Debug")
        list(APPEND _want "nethost.dll")
    endif ()
endif ()

foreach (_f IN LISTS _want)
    if (EXISTS "${BIN_DIR}/${_f}")
        file(COPY "${BIN_DIR}/${_f}" DESTINATION "${_stage}")
    endif ()
endforeach ()

# SDL ships only when linked dynamically; SDL_SHARED_FILE is the resolved shared-lib path (empty for a
# static link). Passed as an absolute genex path so the platform-correct name (SDL3.dll / libSDL3.so.* /
# libSDL3.dylib) needs no reconstruction here.
if (DEFINED SDL_SHARED_FILE AND NOT SDL_SHARED_FILE STREQUAL "" AND EXISTS "${SDL_SHARED_FILE}")
    file(COPY "${SDL_SHARED_FILE}" DESTINATION "${_stage}")
endif ()

# Tracked explicitly rather than by re-globbing the symbol stage: we already know exactly what we copy
# in below, so a glob would only re-derive it.
set(_have_syms FALSE)

# The managed-assembly tree (host assemblies + the shipped first-party plugins now nested under
# managed/plugins). The managed .dll runtime assemblies are platform-neutral .NET, so they ship on every
# platform (into the main stage, .pdb excluded). Their matching .pdb are debug symbols — a separate symbol
# zip only makes sense on Windows (the one platform that also emits a standalone native .pdb), so the
# managed .pdb are packaged into the symbol stage there and simply not shipped elsewhere.
if (IS_DIRECTORY "${BIN_DIR}/managed")
    file(COPY "${BIN_DIR}/managed" DESTINATION "${_stage}" PATTERN "*.pdb" EXCLUDE)
    if (_is_windows)
        file(COPY "${BIN_DIR}/managed" DESTINATION "${_sym}" FILES_MATCHING PATTERN "*.pdb")
        file(GLOB_RECURSE _tree_pdbs "${BIN_DIR}/managed/*.pdb")
        if (_tree_pdbs)
            set(_have_syms TRUE)
        endif ()
    endif ()
endif ()

# The native app symbol file is MSVC's separate .pdb. GCC/Clang embed DWARF in the binary (or emit a
# .dSYM bundle) — not handled here — so on those toolchains there are no separate symbols to ship.
if (_is_windows AND EXISTS "${BIN_DIR}/${APP_NAME}.pdb")
    file(COPY "${BIN_DIR}/${APP_NAME}.pdb" DESTINATION "${_sym}")
    set(_have_syms TRUE)
elseif (NOT _is_windows)
    message(STATUS "dist: ${SYSTEM_NAME} native symbols (DWARF/dSYM) are not bundled; system libmpv not staged")
endif ()

# --format=zip keeps it portable; the relative path keeps the <name>/ folder as the single zip root.
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_zip}" --format=zip "${_name}"
        WORKING_DIRECTORY "${DIST_DIR}"
        RESULT_VARIABLE _tar_rc)
if (NOT _tar_rc EQUAL 0)
    message(FATAL_ERROR "MakeDist: archiving failed (${_tar_rc})")
endif ()
message(STATUS "dist: wrote ${_zip}")

# Only emit the symbol zip if something landed in the symbol stage (nothing does on a DWARF toolchain
# with no managed assemblies) — an empty -pdb.zip would just be noise.
if (_have_syms)
    execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar cf "${_sym_zip}" --format=zip "${_sym_name}"
            WORKING_DIRECTORY "${DIST_DIR}"
            RESULT_VARIABLE _sym_rc)
    if (NOT _sym_rc EQUAL 0)
        message(FATAL_ERROR "MakeDist: symbol archiving failed (${_sym_rc})")
    endif ()
    message(STATUS "dist: wrote ${_sym_zip}")
else ()
    message(STATUS "dist: no separate symbol files to package (skipped ${_sym_name}.zip)")
endif ()

file(REMOVE_RECURSE "${_stage}" "${_sym}")

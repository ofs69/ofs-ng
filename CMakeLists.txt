cmake_minimum_required(VERSION 3.16)

project(ofs-next-generation)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release: /MT (static CRT) — matches libnethost.lib which ships /MT only.
# Debug:   /MDd (dynamic debug CRT) — libnethost.lib has no /MTd counterpart so Debug
#          uses the import lib (nethost.lib) instead; see FindNetHost.cmake.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
endif()

# === Sanitizers (clang/gcc, non-MSVC) ===
# OFS_SANITIZE="address,undefined" (or "thread", etc.) instruments the ofs-ng target ONLY — see the
# guarded block in src/CMakeLists.txt. Third-party libs (SDL, libmpv) stay uninstrumented so their
# shared-library links don't need the sanitizer runtime resolved into the .so (SDL links with
# --no-undefined, which otherwise fails). ASan/LSan interceptors still hook malloc/free
# process-wide, so leaks and overflows in any library are still caught at runtime.
set(OFS_SANITIZE "" CACHE STRING "Sanitizers to enable on the ofs-ng target, e.g. 'address,undefined' (non-MSVC)")

# === AVX / target microarchitecture ===
# ON (default): build the app for x86-64-v3 (AVX2/FMA/BMI) and pull the matching v3 libmpv — this is
# the canonical "ofs-ng" binary. OFF: a baseline SSE2 build with the plain libmpv that still runs on
# pre-AVX CPUs; its binary is named "ofs-ng-no-avx" so both variants can ship side by side. The two are
# separate configurations — configure two build dirs (one per value) to produce both.
option(OFS_AVX "Build for the x86-64-v3 (AVX2) microarchitecture; OFF produces the '-no-avx' build" ON)

# === Clang-Tidy ===
# Tidy analysis is uncached and re-runs on every TU, so it is the slow part of a clang build. It
# runs only in the nightly CI job (see .github/workflows/nightly.yml); set -DOFS_CLANG_TIDY=OFF to
# skip it in a local clang build that happens to have clang-tidy on PATH.
option(OFS_CLANG_TIDY "Run clang-tidy on the ofs-ng target when clang-tidy is found" ON)
if (OFS_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if (CLANG_TIDY_EXE)
        message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
        # We will set this per-target in src/ to avoid linting libraries
    else ()
        message(WARNING "clang-tidy not found.")
    endif ()
else ()
    # Clear any cached path from a prior configure so the src/ gate (if CLANG_TIDY_EXE) stays off.
    unset(CLANG_TIDY_EXE CACHE)
    message(STATUS "clang-tidy disabled (OFS_CLANG_TIDY=OFF)")
endif ()

# === Path-encoding guard ===
# tools/check_path_encoding.py flags the lossy std::filesystem::path<->string conversions that
# silently drop non-ASCII on Windows (ANSI codepage); see src/Util/PathUtil.h and the path rules
# in CLAUDE.md. Wired into the build so a violation fails compilation instead of waiting for a
# manual pre-commit run. The stamp + DEPENDS list keep it incremental: it only re-runs when a
# source file (or the script) changes.
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_FOUND)
    message(STATUS "Python3 found: ${Python3_EXECUTABLE} — path-encoding check enabled")
    file(GLOB_RECURSE OFS_PATH_CHECK_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.h")
    set(OFS_PATH_CHECK_STAMP "${CMAKE_BINARY_DIR}/generated/path_encoding_check.stamp")
    add_custom_command(
        OUTPUT "${OFS_PATH_CHECK_STAMP}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/check_path_encoding.py" "${CMAKE_SOURCE_DIR}/src"
        COMMAND ${CMAKE_COMMAND} -E touch "${OFS_PATH_CHECK_STAMP}"
        DEPENDS ${OFS_PATH_CHECK_SOURCES} "${CMAKE_SOURCE_DIR}/tools/check_path_encoding.py"
        COMMENT "Checking path<->string encoding conventions (check_path_encoding.py)"
        VERBATIM)
    add_custom_target(ofs_check_path_encoding DEPENDS "${OFS_PATH_CHECK_STAMP}")

    # === Unused localization-key guard ===
    # tools/check_unused_strings.py flags keys in tools/localization/strings.toml that no source file
    # references via Str::Key / Tr::Key — dead weight that still has to be translated in every
    # lang/*.toml yet displays nowhere. Re-runs only when strings.toml, a scanned source, or the
    # script changes; the scan covers src/ and tests/ so a key used only by a test isn't flagged.
    file(GLOB_RECURSE OFS_STRINGS_USAGE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_SOURCE_DIR}/tests/*.h")
    set(OFS_UNUSED_STRINGS_STAMP "${CMAKE_BINARY_DIR}/generated/unused_strings_check.stamp")
    add_custom_command(
        OUTPUT "${OFS_UNUSED_STRINGS_STAMP}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/check_unused_strings.py"
                "${CMAKE_SOURCE_DIR}/tools/localization/strings.toml"
                "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests"
        COMMAND ${CMAKE_COMMAND} -E touch "${OFS_UNUSED_STRINGS_STAMP}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/localization/strings.toml" ${OFS_STRINGS_USAGE_SOURCES}
                "${CMAKE_SOURCE_DIR}/tools/check_unused_strings.py"
        COMMENT "Checking for unused localization keys (check_unused_strings.py)"
        VERBATIM)
    add_custom_target(ofs_check_unused_strings DEPENDS "${OFS_UNUSED_STRINGS_STAMP}")
else ()
    message(WARNING "Python3 not found. Path-encoding and unused-string checks skipped; run tools/check_path_encoding.py and tools/check_unused_strings.py by hand.")
    add_custom_target(ofs_check_path_encoding COMMAND ${CMAKE_COMMAND} -E true)
    add_custom_target(ofs_check_unused_strings COMMAND ${CMAKE_COMMAND} -E true)
endif ()


# Build outputs nest under bin/ to keep the project root tidy: the app and its runtime files go to
# bin/<config> (bin/debug, bin/release), test binaries to bin/test (see tests/CMakeLists.txt), and the
# packaged archive to bin/dist (see src/ dist target). Per-config app dirs also mean a Debug and a
# Release build no longer share one directory and clobber/pollute each other's outputs.
#
# Single-config generators (Ninja, used here) get an explicit bin/<config> from CMAKE_BUILD_TYPE.
# Multi-config generators leave CMAKE_BUILD_TYPE empty and append /$<CONFIG> themselves, so the base
# bin/ is correct there (the OFS_RUNTIME_DIR genex in src/CMakeLists.txt mirrors this split).
if (CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
else ()
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _ofs_cfg_lc)
    if (_ofs_cfg_lc STREQUAL "")
        set(_ofs_cfg_lc "debug")  # no build type set → default to a debug-flavored dir
    endif ()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${_ofs_cfg_lc}")
endif ()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(NetHost REQUIRED)

# === Format ===
# A single `format` target that runs clang-format over the C/C++ tree and `dotnet format` over the C#
# plugins solution. Defined here (after find_package(NetHost) sets DOTNET_EXECUTABLE) so the C# step can
# key off it. Each formatter is added only if its tool is present; the target exists if either does.
find_program(CLANG_FORMAT_EXE NAMES "clang-format")
set(FORMAT_COMMANDS "")
if (CLANG_FORMAT_EXE)
    message(STATUS "clang-format found: ${CLANG_FORMAT_EXE}")
    file(GLOB_RECURSE FORMAT_SOURCES
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_SOURCE_DIR}/tests/*.h"
    )
    # Never reformat the vendored Dear ImGui backends (src/Platform/imgui_impl_*); they track
    # upstream style. Same exclusion the test target makes (--excluded_sources "imgui_impl").
    list(FILTER FORMAT_SOURCES EXCLUDE REGEX "imgui_impl")
    # Using a list to avoid command line length limits on Windows
    list(APPEND FORMAT_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E echo "Formatting C/C++ sources (clang-format)..."
        COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_SOURCES})
else ()
    message(WARNING "clang-format not found. 'format' target will skip the C/C++ tree.")
endif ()
if (DOTNET_EXECUTABLE)
    # Clear the Platform env var for the format invocation: the Visual Studio generator sets Platform=x64,
    # which MSBuild reads as a solution config "Debug|x64" that managed.slnx (AnyCPU projects) doesn't
    # define, failing the restore. Unsetting it lets MSBuild pick the solution's default configuration.
    list(APPEND FORMAT_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E echo "Formatting C# projects (dotnet format)..."
        COMMAND ${CMAKE_COMMAND} -E env --unset=Platform
                ${DOTNET_EXECUTABLE} format "${CMAKE_SOURCE_DIR}/managed/managed.slnx")
else ()
    message(WARNING "dotnet not found. 'format' target will skip the C# plugins.")
endif ()
if (FORMAT_COMMANDS)
    add_custom_target(format ${FORMAT_COMMANDS} COMMENT "Running formatters" VERBATIM)
else ()
    message(WARNING "Neither clang-format nor dotnet found. 'format' target will not be available.")
endif ()

# Enable testing at the top level so ctest finds a CTestTestfile.cmake in the
# build root. add_test() calls live in tests/CMakeLists.txt; without this the
# tests register only in the tests/ subdir scope and `ctest --test-dir <build>`
# reports "No tests were found".
enable_testing()

# Baked-in trust hashes: the SHA-256 of each shipped plugin DLL and each managed host assembly is
# compiled into the app at build time (see cmake/GenBakedHashes.cmake + managed/CMakeLists.txt).
# First-party plugins load without the third-party trust prompt (a tampered/imposter DLL under the
# same name still prompts); the managed host assemblies must match exactly or loading is refused.
# Shared here so the generator (managed/), the consumer (src/) and the tests all agree on the lists,
# paths and roots.
set(OFS_FIRST_PARTY_PLUGINS "Ofs.Core")
set(OFS_MANAGED_ASSEMBLIES "Ofs.Api;Ofs.PluginHost;Ofs.ScriptHost;Ofs.HostServices")
set(OFS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(OFS_BAKED_HASHES_HEADER "${OFS_GENERATED_DIR}/OfsBakedHashes.h")
# Generated localization sources. Emitted from tools/localization/strings.toml by localization_gen into
# the build tree (see tools/localization/CMakeLists.txt) — not committed to the src tree. Shared here so
# both the app (src/) and the tests/ targets reference the same paths.
set(OFS_STRINGS_HDR "${OFS_GENERATED_DIR}/Localization/StringsGenerated.h")
set(OFS_STRINGS_SRC "${OFS_GENERATED_DIR}/Localization/StringsGenerated.cpp")
# Shipped first-party plugins live UNDER the managed dir (bin/managed/plugins), not in a top-level
# bin/plugins next to the executable. A bin/plugins folder reads as "drop your plugins here", but the
# base root is loaded first-party-by-name only and never scanned, so a user plugin dropped there would
# silently never load. Nesting under managed/ leaves the writable <pref>/plugins as the only
# user-facing plugins folder. PluginManager's base root mirrors this (getBasePath()/managed/plugins).
set(OFS_MANAGED_OUTPUT_ROOT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed")
set(OFS_PLUGIN_OUTPUT_ROOT "${OFS_MANAGED_OUTPUT_ROOT}/plugins")
# Baseline at configure time so the header always exists for compilation/IDE indexing (and for a
# dotnet-less build that never reaches the build-time step). Real hashes are baked at build time.
execute_process(COMMAND ${CMAKE_COMMAND}
        -DOUT_HEADER=${OFS_BAKED_HASHES_HEADER}
        -DPLUGIN_ROOT=${OFS_PLUGIN_OUTPUT_ROOT}
        "-DFIRST_PARTY=${OFS_FIRST_PARTY_PLUGINS}"
        -DMANAGED_ROOT=${OFS_MANAGED_OUTPUT_ROOT}
        "-DMANAGED=${OFS_MANAGED_ASSEMBLIES}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GenBakedHashes.cmake")

# Shipped runtime assets: data/ (which now holds the script library under data/scripts/lib/) and lang/
# are packed into one deflate-compressed data.pak and loaded at runtime through ofs::res
# (src/Util/Resources). This replaces both the loose data/ file copy and the former bespoke header embeds
# (OfsEmbedded{Themes,Simulator}.h): fonts, the simulator model, the base Dark/Light themes, shipped
# translations and the shipped script library all now live in this one archive. User
# themes/translations/scripts still live under <pref>/ and override the shipped ones. GLOB_RECURSE +
# CONFIGURE_DEPENDS re-globs at build time, so the archive re-packs whenever any asset is added or edited;
# the staging rule in src/CMakeLists.txt copies it next to the binary as a tracked build output (the fix
# for the prior "data not always copied" bug). See src/CMakeLists.txt for the OFS_ASSETS_FALLBACK_DIR dev
# fallback.
set(OFS_ASSETS_PAK "${OFS_GENERATED_DIR}/data.pak")
file(GLOB_RECURSE OFS_ASSET_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/data/*" "${CMAKE_SOURCE_DIR}/lang/*")
add_custom_command(
        OUTPUT "${OFS_ASSETS_PAK}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${OFS_ASSETS_PAK}
        -P "${CMAKE_SOURCE_DIR}/cmake/PackAssets.cmake"
        DEPENDS ${OFS_ASSET_FILES} "${CMAKE_SOURCE_DIR}/cmake/PackAssets.cmake"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Packing data/ and lang/ into data.pak"
        VERBATIM)
add_custom_target(ofs_pack_assets DEPENDS "${OFS_ASSETS_PAK}")

# Embedded build identity: the git commit hash (long + short) and nearest tag, compiled into the
# binary (see cmake/GenBuildInfo.cmake + src/main.cpp). Baseline at configure time; refreshed on every
# build by the ofs_build_info custom target — the checked-out commit can move without a reconfigure,
# and the script rewrites the header only when the revision actually changes.
find_package(Git QUIET)
set(OFS_BUILD_INFO_HEADER "${OFS_GENERATED_DIR}/OfsBuildInfo.h")
execute_process(COMMAND ${CMAKE_COMMAND}
        -DOUT_HEADER=${OFS_BUILD_INFO_HEADER}
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -DWORK_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GenBuildInfo.cmake")
add_custom_target(ofs_build_info
        COMMAND ${CMAKE_COMMAND}
        -DOUT_HEADER=${OFS_BUILD_INFO_HEADER}
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -DWORK_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GenBuildInfo.cmake"
        COMMENT "Refreshing OfsBuildInfo.h (git identity)"
        VERBATIM)

# Every compilable source under src/. GLOB_RECURSE + CONFIGURE_DEPENDS re-globs at build time and
# reconfigures when a .cpp is added or removed, so a new src file is picked up without hand-editing a
# source list. Consumed by both the app (src/) and the full-app test executables (tests/ — ui-tests),
# which is why it is defined here in the shared parent scope. The generated localization
# .cpp lives in the build tree (OFS_STRINGS_SRC), outside this glob, and is appended per target.
# NOTE: the unit/plugin test binaries deliberately compile a hand-picked subset (no window, no video)
# and intentionally do NOT use this glob.
file(GLOB_RECURSE OFS_SRC_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# Precompiled-header set, shared by every target that compiles the src tree (the app in src/, and the
# object libs + test binaries in tests/). Nearly all 68 src TUs pull in imgui.h, nlohmann/json.hpp,
# glm and the same handful of STL headers; each was being fully re-parsed per TU, which dominates a
# clean Release compile. Parsing them once into a per-target PCH is the win. Applied with the
# target's own flags/defines, so each target (plain-imgui vs imgui_te flavor) gets a self-consistent
# PCH — applied via the ofs_target_pch() helper below, called from src/ and tests/.
#
# DELIBERATELY third-party + STL ONLY — all effectively immutable. A frequently-edited project header
# in the PCH would make every edit to it trigger a full-target rebuild, wrecking incremental builds;
# these never change, so the PCH stays valid across edits and the speedup is pure. Keep it that way.
set(OFS_PCH_HEADERS
        # C++ standard library — heavy and included almost everywhere.
        <algorithm> <array> <chrono> <cmath> <cstddef> <cstdint> <functional>
        <map> <memory> <optional> <ranges> <string> <string_view> <unordered_map> <vector>
        # Heavy third-party headers (stable, vendored). json.hpp especially is a ~25k-line single
        # header; parsing it once instead of in all 15 of its consumers is most of the gain.
        <imgui.h>
        <imgui_internal.h>  # must follow imgui.h; ~4k vendored lines, pulled into ~12 TUs
        <nlohmann/json.hpp>
        <glm/glm.hpp>
        <spdlog/fmt/fmt.h>)

# Apply the shared PCH to a target — UNLESS clang-tidy is active. clang-tidy parses with clang and
# rejects a PCH it didn't make: a GCC build produces a .gch clang refuses ("not a clang PCH file"), and
# even a clang-built one trips -Winvalid-pch on a tidy/clang version skew — and our --warnings-as-errors=*
# turns that into a hard error. So skip the PCH when tidy runs (the nightly lint job, or a local tidy
# build); its uncached analysis dominates that build anyway, so the PCH would save little. Every other
# build — CI build-test, release, normal dev, all of Windows — has tidy off and keeps the PCH.
function(ofs_target_pch target)
    if (CLANG_TIDY_EXE)
        return()
    endif ()
    target_precompile_headers(${target} PRIVATE ${OFS_PCH_HEADERS})
endfunction()

add_subdirectory("lib/")
add_subdirectory("tools/localization/")
add_subdirectory("managed/")
add_subdirectory("src/")
add_subdirectory("tests/")

set(_build_done_deps ofs-ng)
if (OFS_BUILD_TESTS)
    # ofs-ui-tests-headless is always built; the real-GL ofs-ui-tests only when OFS_UI_TESTS_GL
    # is ON. CI configures -DOFS_UI_TESTS_GL=OFF, so depending on ofs-ui-tests there would be a
    # missing-target error ('no known rule to make it').
    list(APPEND _build_done_deps ofs-unit-tests ofs-plugin-tests ofs-ui-tests-headless)
    if (OFS_UI_TESTS_GL)
        list(APPEND _build_done_deps ofs-ui-tests)
    endif ()
endif ()
add_custom_target(build-done ALL
        COMMAND ${CMAKE_COMMAND} -E true
        DEPENDS ${_build_done_deps}
        COMMENT "==> ofs-ng + all tests built successfully.")


project(ofs-ng)

# The full src tree (globbed in the root CMakeLists, OFS_SRC_FILES — includes main.cpp) plus the
# generated localization .cpp, which lives in the build tree and so is outside the glob.
set(OFS_SOURCES
        ${OFS_SRC_FILES}
        ${OFS_STRINGS_SRC}  # generated into the build tree (tools/localization/), depends on ofs_strings_gen
)

# The generated .cpp lives in the build tree; mark it GENERATED so CMake doesn't require it to exist
# at configure time. The ofs_strings_gen target (tools/localization/) owns the rule that produces it.
set_source_files_properties("${OFS_STRINGS_SRC}" PROPERTIES GENERATED TRUE)

if (WIN32)
    list(APPEND OFS_SOURCES icon.rc)
    add_executable(${PROJECT_NAME} WIN32 ${OFS_SOURCES})
else ()
    add_executable(${PROJECT_NAME} ${OFS_SOURCES})
endif ()

add_dependencies(${PROJECT_NAME} ofs_strings_gen ofs_build_info ofs_check_path_encoding ofs_check_unused_strings)

# Resources.cpp falls back to reading assets straight from the source tree when no data.pak is staged
# next to the binary (dev builds run from a fresh tree). Shipped builds always find the staged zip first.
target_compile_definitions(${PROJECT_NAME} PRIVATE OFS_ASSETS_FALLBACK_DIR="${CMAKE_SOURCE_DIR}")
if (TARGET Ofs.PluginHost)
    add_dependencies(${PROJECT_NAME} Ofs.PluginHost)
endif ()
if (TARGET SamplePlugin)
    add_dependencies(${PROJECT_NAME} SamplePlugin)
endif ()
if (TARGET Ofs.Core)
    add_dependencies(${PROJECT_NAME} Ofs.Core)
endif ()
# Generated baked hashes (PluginManager.cpp / ScriptSystem.cpp include OfsBakedHashes.h). The target
# exists only when the managed assemblies are built (dotnet present); without it the
# configure-time baseline header is used.
if (TARGET ofs_baked_hashes)
    add_dependencies(${PROJECT_NAME} ofs_baked_hashes)
endif ()


target_link_libraries(${PROJECT_NAME} PUBLIC SDL3::SDL3 imgui imgui_stdlib glad2 spdlog nlohmann_json::nlohmann_json glm stb NetHost::NetHost imnodes miniz Bitsery::bitsery concurrentqueue thread_pool tomlplusplus::tomlplusplus)

# dbghelp provides MiniDumpWriteDump for the crash handler (Windows only).
if (WIN32)
    target_link_libraries(${PROJECT_NAME} PUBLIC dbghelp)
endif ()

# Linux: fold the C++ runtime into the binary so the only DT_NEEDED entries left are libm, libc and the
# loader. Everything else this app touches is already dlopen'd rather than linked — SDL3 links statically
# and dlopens each of its backends (X11/Wayland/GL/ALSA/Pulse/udev/dbus) by soname, libmpv comes from
# MpvLoader's SDL_LoadObject, and hostfxr from libnethost — so libstdc++/libgcc_s were the last host
# libraries we imposed. Dropping them also drops the binary's GLIBCXX_* symbol-version floor, which is
# what lets one AppImage run against an older distro's C++ runtime (see tools/appimage/).
#
# Safe despite CoreCLR loading its own libstdc++.so.6 into the same process: nothing but the plugin C ABI
# crosses that boundary, so the two runtimes never exchange C++ objects, exceptions or typeinfo. The
# plugin/script suite (real CoreCLR + Roslyn) passes against a statically linked binary.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
endif ()


target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/lib/libmpv/include"
        "${OFS_GENERATED_DIR}"  # generated OfsBakedHashes.h (SYSTEM so clang-tidy skips it)
)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

# Precompile the stable heavy headers (see OFS_PCH_HEADERS in the root CMakeLists). The vendored Dear
# ImGui backends are force-include-sensitive (their own include ordering) and are not ours to reshape,
# so they opt out — consistent with the clang-format/clang-tidy exclusions they already carry.
ofs_target_pch(${PROJECT_NAME})
set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/imgui_impl_opengl3.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/imgui_impl_sdl3.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Platform/imgui_impl_null.cpp"
        PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)

target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
)

# AVX build: compile for x86-64-v3 (AVX2/FMA/BMI), matching the v3 libmpv. The baseline build
# (-DOFS_AVX=OFF) stays SSE2 so it runs on pre-AVX CPUs, and its binary is suffixed "-compat" so both
# variants can ship side by side. See OFS_AVX in the root CMakeLists.
#
# x86-64-v3 / AVX2 are meaningful only on a 64-bit x86 target. On ARM (Apple Silicon, aarch64) the
# flag names don't exist and would hard-error, so the microarch switch is a no-op there — OFS_AVX
# still only governs the binary suffix. The v3-vs-baseline libmpv archive is switched separately, and
# only on Windows (see the WIN32 mpv block below); other platforms use the host's system libmpv.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ofs_arch_lc)
if (_ofs_arch_lc MATCHES "^(amd64|x86_64|x64)$")
    set(_ofs_x86_64 TRUE)
else ()
    set(_ofs_x86_64 FALSE)
endif ()

if (OFS_AVX)
    if (_ofs_x86_64)
        target_compile_options(${PROJECT_NAME} PRIVATE
                $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2>
                $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-march=x86-64-v3>)
    endif ()
else ()
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${PROJECT_NAME}-compat")
endif ()

# Instrument only this target (not the bundled libs); see OFS_SANITIZE in the root CMakeLists.txt.
if (OFS_SANITIZE AND NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
            -fsanitize=${OFS_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=${OFS_SANITIZE})
endif ()

# Ship optimized Release builds that still carry debug info, so crash-dump minidumps
# can be symbolicated. Archive the resulting ofs-ng.pdb for every release you publish.
if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:/Zi>)
    # /DEBUG emits the PDB; /OPT:REF and /OPT:ICF undo the size/perf cost that /DEBUG
    # otherwise disables, keeping the binary as lean as a plain Release link.
    target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>)
endif ()

if (CLANG_TIDY_EXE)
    set(OFS_CLANG_TIDY_ARGS "${CLANG_TIDY_EXE};--system-headers=0;--warnings-as-errors=*")
    if (MSVC)
        list(APPEND OFS_CLANG_TIDY_ARGS "--extra-arg=/EHsc")
    endif ()
    set_target_properties(${PROJECT_NAME} PROPERTIES
            CXX_CLANG_TIDY "${OFS_CLANG_TIDY_ARGS}"
    )
    set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/Platform/imgui_impl_opengl3.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Platform/imgui_impl_sdl3.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Platform/imgui_impl_null.cpp"
            "${OFS_STRINGS_SRC}"
            PROPERTIES
            SKIP_LINTING TRUE
    )
endif ()


# Distribution archive: package this build's runtime artifacts (app, runtime DLLs, data.pak, plugins,
# managed assemblies) into dist/<platform>-<app>-<tag>+<shorthash>[-compat].zip, with the .pdb symbols
# split into a sibling …-pdb.zip so the shipped archive stays lean. MakeDist names each artifact
# explicitly rather than zipping bin/ wholesale, because bin/<config> is shared by both AVX variants, so
# a blanket copy would sweep in the other variant's exe/pdb. APP_FILE/SYSTEM_NAME/SDL_SHARED_FILE are
# passed as resolved genexes so the binary extension, platform tag and SDL shared lib are correct
# cross-platform; AVX guarantees the "-compat" marker. The localization_gen host tool is a build-time
# generator and is never listed. Not part of ALL — run it explicitly: `cmake --build <dir> --target
# dist`. The git tag and short commit hash are resolved when the target runs (not at configure), so the
# archive name always tracks the checked-out revision.
add_custom_target(dist
        COMMAND ${CMAKE_COMMAND}
        -DBIN_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>
        -DDIST_DIR=${CMAKE_SOURCE_DIR}/bin/dist
        -DAPP_NAME=$<TARGET_FILE_BASE_NAME:${PROJECT_NAME}>
        -DAPP_FILE=$<TARGET_FILE_NAME:${PROJECT_NAME}>
        -DCONFIG=$<CONFIG>
        -DSYSTEM_NAME=${CMAKE_SYSTEM_NAME}
        -DAVX=${OFS_AVX}
        # Resolved SDL shared-lib path when linked dynamically; empty (static link → no DLL to ship).
        "-DSDL_SHARED_FILE=$<$<NOT:$<BOOL:${OFS_SDL_STATIC}>>:$<TARGET_FILE:SDL3::SDL3>>"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -DWORK_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/MakeDist.cmake"
        COMMENT "Packaging dist/ archive"
        VERBATIM)
add_dependencies(dist ${PROJECT_NAME})


# Setup mpv
if (WIN32)
    # AVX build pulls the x86-64-v3 libmpv (AVX2/FMA/BMI); the baseline build pulls the plain SSE2 one.
    # See OFS_AVX in the root CMakeLists. The archive is cached per-variant by name so flipping OFS_AVX
    # never reuses the wrong one.
    if (OFS_AVX)
        set(_mpv_url "https://sourceforge.net/projects/mpv-player-windows/files/libmpv/mpv-dev-x86_64-v3-20260531-git-13a3e3a.7z/download")
        set(_mpv_tag "avx")
    else ()
        set(_mpv_url "https://sourceforge.net/projects/mpv-player-windows/files/libmpv/mpv-dev-x86_64-20260531-git-13a3e3a.7z/download")
        set(_mpv_tag "noavx")
    endif ()
    set(_mpv_archive "${CMAKE_BINARY_DIR}/mpv-dev-${_mpv_tag}.7z")

    # If a previous configure of this build dir used the other variant, drop the extracted DLL so the
    # correct one is re-extracted below (the loader always opens the canonical name "libmpv-2.dll").
    set(_mpv_stamp "${CMAKE_BINARY_DIR}/mpv-variant.txt")
    if (EXISTS "${_mpv_stamp}")
        file(READ "${_mpv_stamp}" _mpv_prev)
    else ()
        set(_mpv_prev "")
    endif ()
    if (NOT _mpv_prev STREQUAL _mpv_tag)
        file(REMOVE "${CMAKE_BINARY_DIR}/libmpv-2.dll")
        file(WRITE "${_mpv_stamp}" "${_mpv_tag}")
    endif ()

    if (NOT EXISTS "${CMAKE_BINARY_DIR}/7zr.exe")
        file(DOWNLOAD "https://www.7-zip.org/a/7zr.exe" "${CMAKE_BINARY_DIR}/7zr.exe" SHOW_PROGRESS)
    endif ()
    if (NOT EXISTS "${CMAKE_BINARY_DIR}/libmpv-2.dll")
        if (NOT EXISTS "${_mpv_archive}")
            file(DOWNLOAD "${_mpv_url}" "${_mpv_archive}" SHOW_PROGRESS)
        endif ()
        add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
                COMMAND "${CMAKE_BINARY_DIR}/7zr.exe" "x" "-y" "${_mpv_archive}" "libmpv-2.dll"
                WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                BYPRODUCTS "${CMAKE_BINARY_DIR}/libmpv-2.dll")
    endif ()

    # copy libmpv-2.dll to bin dir
    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/libmpv-2.dll")
    # Debug uses the import lib (nethost.lib) and needs nethost.dll at runtime.
    # Release statically links libnethost.lib (/MT) — no DLL needed.
    if (NetHost_RUNTIME_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E
                $<IF:$<CONFIG:Debug>,copy_if_different,true>
                "${NetHost_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/nethost.dll")
    endif ()
endif ()


# Setup ffmpeg / ffprobe (Windows only). We shell out to these two CLIs and bundle them next to the app
# on Windows; other platforms expect them on PATH via the system package manager, so nothing is fetched
# there. The "git-essentials" build is a rolling archive (the URL is not version-pinned), and the two
# exes live under a dated <folder>/bin/ inside it, so we extract them by name with `e -r` (flatten +
# recurse) rather than a fixed path that would encode the folder's date. The CI cache pins whatever was
# first downloaded. (essentials, not full: ~33 MB vs ~164 MB — full only adds long-tail codecs/filters
# we don't use.)
if (WIN32)
    set(_ffmpeg_url "https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-essentials.7z")
    set(_ffmpeg_archive "${CMAKE_BINARY_DIR}/ffmpeg-git-essentials.7z")

    # 7zr.exe is normally already fetched by the mpv block above; guard so this block stands on its own.
    if (NOT EXISTS "${CMAKE_BINARY_DIR}/7zr.exe")
        file(DOWNLOAD "https://www.7-zip.org/a/7zr.exe" "${CMAKE_BINARY_DIR}/7zr.exe" SHOW_PROGRESS)
    endif ()
    if (NOT EXISTS "${CMAKE_BINARY_DIR}/ffmpeg.exe" OR NOT EXISTS "${CMAKE_BINARY_DIR}/ffprobe.exe")
        if (NOT EXISTS "${_ffmpeg_archive}")
            file(DOWNLOAD "${_ffmpeg_url}" "${_ffmpeg_archive}" SHOW_PROGRESS)
        endif ()
        add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
                COMMAND "${CMAKE_BINARY_DIR}/7zr.exe" "e" "-y" "${_ffmpeg_archive}" "-r" "ffmpeg.exe" "ffprobe.exe"
                WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                BYPRODUCTS "${CMAKE_BINARY_DIR}/ffmpeg.exe" "${CMAKE_BINARY_DIR}/ffprobe.exe")
    endif ()

    # copy ffmpeg.exe / ffprobe.exe next to the app binary
    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/ffmpeg.exe" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/ffprobe.exe" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/ffprobe.exe")
endif ()



option(OFS_BUILD_TESTS "Build test targets" ON)
# The real-GL UI test binary (ofs-ui-tests) opens a window and needs a GL context. The
# headless null-backend binary (ofs-ui-tests-headless, built with OFS_HEADLESS) needs neither and is
# always built. CI sets this OFF so it builds and runs ONLY the headless variant (no Mesa/EGL/Xvfb);
# a local build keeps it ON to also get the real-GL binary for pixel/GL validation.
option(OFS_UI_TESTS_GL "Build the real-GL UI test binary (ui-tests)" ON)

if (NOT OFS_BUILD_TESTS)
    return()
endif ()

# Test executables live under bin/test/, separate from the app's per-config bin/<config>/. CMake
# creates a target's RUNTIME_OUTPUT_DIRECTORY eagerly at generate time (no genex defers it in a
# single-config generator), so pointing the test targets at the app dir left empty ui-tests and
# plugin-tests subdirs sitting next to the shipped app after a bare configure. Their own
# subtree keeps the shipped dir to exactly what ships.
set(OFS_TEST_BIN_DIR "${CMAKE_SOURCE_DIR}/bin/test")

# Stage the runtime DLLs the OS resolves next to a test binary. Each test target lives in its own dir
# under bin/test/ (not beside the app in bin/<config>), so the DLLs the app normally finds there must be
# copied in per target. SDL3 is always staged (a static SDL build resolves the genex to the .lib, which
# copy_if_different harmlessly stages); MPV/NETHOST are opt-in so a target only gets what it links:
#   MPV     — the full-window UI binary (ui-tests) loads libmpv; Windows only.
#   NETHOST — binaries that host CoreCLR (plugin-tests, ui-tests) need nethost.dll in Debug,
#             where NetHost links the import lib; Windows only, and only when the DLL was found.
function(ofs_stage_runtime_dlls target)
    cmake_parse_arguments(ARG "MPV;NETHOST" "" "" ${ARGN})
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>/")
    if (WIN32 AND ARG_MPV)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:${target}>/libmpv-2.dll")
    endif ()
    if (WIN32 AND ARG_NETHOST AND NetHost_RUNTIME_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NetHost_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>/nethost.dll")
    endif ()
endfunction()

# Stage everything a full-window UI test binary resolves at runtime, next to ${target}: the asset pak
# (fonts/themes/glb/lang), an empty plugins/ fixture, the runtime DLLs, and — when the .NET SDK is
# present — the managed host + the real C# test plugin (with Ofs.Core staged aside for on-demand load).
# Shared by ofs-ui-tests (real GL) and ofs-ui-tests-headless (null backend) so the two are byte-identical
# at runtime and differ only in how they render (real GL vs none).
function(ofs_setup_ui_test_runtime target)
    # Stage data.pak next to the binary so the app loads fonts/themes/glb/lang from it, exactly as
    # shipped. ofs::res still falls back to the source tree (OFS_ASSETS_FALLBACK_DIR) if it goes missing.
    add_custom_command(TARGET ${target} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OFS_ASSETS_PAK}" "$<TARGET_FILE_DIR:${target}>/data.pak")

    # Reset the plugins/ fixture dir (next to the binary) to empty. It holds only the staged
    # Ofs.TestPlugin (a fixture suite_plugin_ui copies into the pref dir on demand); the app's base root
    # is managed/plugins, kept empty by the POST_BUILD strip below.
    add_custom_command(TARGET ${target} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:${target}>/plugins"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/plugins")

    # Runtime DLLs the OS resolves next to the executable (the full-window binary loads libmpv and hosts CoreCLR).
    ofs_stage_runtime_dlls(${target} MPV NETHOST)

    # Stage the real C# test plugin + managed host so suite_plugins' binding round-trip can boot CoreCLR
    # and load a genuine plugin. Reuse the artifact already built by the plugin-tests Ofs.TestPlugin
    # target. loadPlugins() never scans the base dir, so this stays inert until a test copies it into the
    # pref dir — every other ui-test remains plugin-free.
    if (DOTNET_EXECUTABLE AND TARGET Ofs.PluginHost AND TARGET Ofs.TestPlugin)
        add_dependencies(${target} Ofs.TestPlugin Ofs.PluginHost)
        if (TARGET Ofs.ScriptHost)
            add_dependencies(${target} Ofs.ScriptHost)
        endif ()
        # Strip managed/plugins after copying so the base root (getBasePath()/managed/plugins) starts
        # EMPTY — otherwise Ofs.Core would startup-load for every suite. suite_plugin_ui stages Ofs.Core
        # back in on demand (core-plugin staging below).
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${OFS_TEST_BIN_DIR}/plugin-tests/plugins/Ofs.TestPlugin"
                "$<TARGET_FILE_DIR:${target}>/plugins/Ofs.TestPlugin"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed" "$<TARGET_FILE_DIR:${target}>/managed"
                COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:${target}>/managed/plugins")

        # Ofs.Core is FIRST-PARTY: loadPlugins() loads it by name from the base plugins dir. Leaving it
        # staged there would auto-load it at startup for every suite, so stage it into core-plugin/ (a dir
        # loadPlugins never scans); suite_plugin_ui copies it into managed/plugins right before its single
        # loadPlugins() call so it loads on demand.
        if (TARGET Ofs.Core)
            add_dependencies(${target} Ofs.Core)
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${OFS_PLUGIN_OUTPUT_ROOT}/Ofs.Core"
                    "$<TARGET_FILE_DIR:${target}>/core-plugin/Ofs.Core")
        endif ()
    endif ()
endfunction()

# The localization sources are generated into the build tree by ofs_strings_gen (tools/localization/).
# Mark them GENERATED so CMake doesn't require them at configure time; targets below that compile
# OFS_STRINGS_SRC add_dependencies(ofs_strings_gen) so the rule runs first.
set_source_files_properties("${OFS_STRINGS_SRC}" PROPERTIES GENERATED TRUE)

# The vendored Dear ImGui backends opt out of the precompiled header (force-include-sensitive; not ours
# to reshape) — mirror the same opt-out src/ makes. Source-file properties are per-directory scope, so
# this must be set here too for the tests/ targets that compile these files (ofs-ui-tests and
# ofs-ui-tests-headless, via the OFS_SRC_FILES glob). The PCH set itself is OFS_PCH_HEADERS (root
# CMakeLists); applied per target below.
set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/src/Platform/imgui_impl_opengl3.cpp"
        "${CMAKE_SOURCE_DIR}/src/Platform/imgui_impl_sdl3.cpp"
        "${CMAKE_SOURCE_DIR}/src/Platform/imgui_impl_null.cpp"
        PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)

# ──────────────────────────────────────────────────────────────────────────────
# Shared object library — compile the common app core once and link it into the
# binaries that share its ImGui flavor, instead of recompiling it per binary.
#
# The hard partition is the ImGui build flavor. imgui_te defines
# IMGUI_ENABLE_TEST_ENGINE, which changes sizeof(ImGuiIO) (see lib/CMakeLists.txt) —
# so object files compiled against plain `imgui` MUST NOT be linked into a binary
# that uses `imgui_te`, or the IMGUI_CHECKVERSION struct-layout assert fires.
#
# Only the two window-free binaries (ofs-unit-tests + ofs-plugin-tests) share an
# object lib — ofs-obj-base, the plain-imgui core both compile. The two full-window
# binaries (ofs-ui-tests + ofs-ui-tests-headless) use imgui_te but differ from each
# other by OFS_HEADLESS (null vs real GL backend), so they share nothing and each
# compiles the whole app (OFS_APP_SOURCES) itself.
#
# PathUtil.cpp is never placed in a shared object lib: it bakes OFS_TEST_PREF_SUBDIR,
# whose value differs per binary, so each binary compiles its own copy (the window
# binaries via OFS_APP_SOURCES, which includes it).

# The plain-imgui core shared by the two window-free binaries (unit + plugin).
# These are the only src files BOTH compile; each binary still compiles its own
# extras (doc model for unit, plugin host for plugin) and PathUtil directly.
# Resources.cpp (and only it) reads OFS_ASSETS_FALLBACK_DIR, so the macro lives here.
set(OFS_OBJ_BASE_SRC
        ${CMAKE_SOURCE_DIR}/src/Core/EventQueue.cpp
        ${CMAKE_SOURCE_DIR}/src/Core/ScriptProject.cpp
        ${CMAKE_SOURCE_DIR}/src/Core/StandardAxis.cpp
        ${CMAKE_SOURCE_DIR}/src/Core/FunscriptMetadata.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/JobSystem.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/ProcessingSystem.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/ScriptHeader.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/BindingSystem.cpp
        ${CMAKE_SOURCE_DIR}/src/Localization/Translator.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/Coro.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/FrameAllocator.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/Log.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/FileUtil.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/Resources.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/TimeUtil.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/FuzzyMatch.cpp
        ${OFS_STRINGS_SRC}  # generated; see tools/localization/CMakeLists.txt
)
add_library(ofs-obj-base OBJECT ${OFS_OBJ_BASE_SRC})
target_include_directories(ofs-obj-base PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ofs-obj-base SYSTEM PRIVATE ${OFS_GENERATED_DIR})
target_compile_definitions(ofs-obj-base PRIVATE
        OFS_ASSETS_FALLBACK_DIR="${CMAKE_SOURCE_DIR}")
# PRIVATE so the plain-imgui usage requirement does NOT leak to consumers — the
# binaries name their own imgui flavor. Linking the lib targets only pulls in their
# INTERFACE include dirs for this lib's own compile.
target_link_libraries(ofs-obj-base PRIVATE
        imgui imnodes SDL3::SDL3 nlohmann_json::nlohmann_json glm::glm spdlog::spdlog
        concurrentqueue thread_pool tomlplusplus::tomlplusplus miniz Bitsery::bitsery)
add_dependencies(ofs-obj-base ofs_strings_gen)  # generated StringsGenerated.{h,cpp}
ofs_target_pch(ofs-obj-base)
if (MSVC)
    target_compile_options(ofs-obj-base PRIVATE /WX /wd4702)
endif ()

# OFS_APP_SOURCES — the whole app minus main.cpp, plus generated strings. Both full-window binaries
# (ofs-ui-tests + ofs-ui-tests-headless) compile it directly; they can't share an object lib (their
# OFS_HEADLESS flavor differs) and each bakes its own OFS_TEST_PREF_SUBDIR into PathUtil.cpp (in the glob).
set(OFS_APP_SOURCES ${OFS_SRC_FILES})
list(REMOVE_ITEM OFS_APP_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")
list(APPEND OFS_APP_SOURCES "${OFS_STRINGS_SRC}")

# --- Unit tests (no window) ---
add_executable(ofs-unit-tests
        test_main_unit.cpp
        helpers/TestProject.cpp
        helpers/FixtureCompare.cpp

        # Phase 2 — core data structure tests
        core/test_vector_set.cpp
        core/test_processing_region.cpp
        core/test_time_slot.cpp
        core/test_processing_graph_ops.cpp
        core/test_frame_allocator.cpp
        core/test_scene_view.cpp
        core/test_standard_axis.cpp
        scenegraph/test_vr_camera.cpp

        # Phase 3 — EventQueue and ScriptProject tests
        core/test_event_queue.cpp
        core/test_script_project.cpp

        # Phase 4 — service integration tests
        services/test_undo_system.cpp
        services/test_undo_snapshot_codec.cpp
        services/test_snapshot_history.cpp
        services/test_job_await.cpp
        services/test_job_system.cpp
        services/test_project_manager.cpp
        services/test_project_manager_handlers.cpp
        services/test_axis_grouping.cpp
        services/test_modal_manager.cpp
        services/test_processing_system.cpp
        services/test_native_effects.cpp
        services/test_script_nodes.cpp
        services/test_graph_trust.cpp
        services/test_binding_system.cpp
        services/test_command_providers.cpp
        services/test_custom_command_store.cpp

        # Phase 5 — format / settings serialization tests
        format/test_app_settings.cpp
        format/test_layout_store.cpp
        ui/test_theme.cpp

        # Round 2 - ProjectManager mutation handlers, format, util, localization
        services/test_project_bookmarks.cpp
        services/test_project_sceneview.cpp
        services/test_project_metadata.cpp
        services/test_project_selection.cpp
        services/test_project_regions.cpp
        services/test_active_axis.cpp
        format/test_funscript.cpp
        format/test_graph_preset.cpp
        format/test_project.cpp
        format/test_backup_archive.cpp
        core/test_funscript_metadata.cpp
        util/test_time_util.cpp
        util/test_benchmark.cpp
        util/test_path_util.cpp
        util/test_file_fingerprint.cpp
        util/test_json_util.cpp
        util/test_fuzzy_match.cpp
        util/test_fault_throttle.cpp
        util/test_semver.cpp
        util/test_pref_version_guard.cpp
        services/test_command_registry.cpp
        services/test_video_transcoder.cpp
        services/test_waveform_peaks.cpp
        video/test_playback_cursor.cpp
        localization/test_translator.cpp

        # Unit-only src (the shared plain-imgui core comes from ofs-obj-base, linked
        # below). These document-model files are compiled only by this binary; the
        # plugin binary does not use them and the ui binaries are a different ImGui
        # flavor, so there is no shared object lib for them. PathUtil bakes this
        # binary's OFS_TEST_PREF_SUBDIR value; ModalManager is built WITHOUT
        # OFS_TEST_ENGINE (the engine copy rides the full-window binaries' app compile).
        ${CMAKE_SOURCE_DIR}/src/Scenegraph/VrCamera.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/UndoSystem.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/SnapshotCodec.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/ProjectManager.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/NavigatorRouter.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/EditIntentRouter.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/SelectIntentRouter.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/CommandProviders.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/CustomCommandStore.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/CustomCommandTemplate.cpp
        ${CMAKE_SOURCE_DIR}/src/Format/Funscript.cpp
        ${CMAKE_SOURCE_DIR}/src/Format/AppSettings.cpp
        ${CMAKE_SOURCE_DIR}/src/Format/BackupArchive.cpp
        ${CMAKE_SOURCE_DIR}/src/Format/LayoutStore.cpp
        ${CMAKE_SOURCE_DIR}/src/Format/Project.cpp
        ${CMAKE_SOURCE_DIR}/src/UI/Theme.cpp
        ${CMAKE_SOURCE_DIR}/src/UI/ProcessingGraphOps.cpp
        ${CMAKE_SOURCE_DIR}/src/UI/ModalManager.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/PathUtil.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/PrefVersionGuard.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/FileFingerprint.cpp
        # Pure transcode helpers (ofs::transcode) live in VideoTranscoder.cpp; Subprocess.cpp resolves
        # the Process/runCaptured symbols its worker references (no process is spawned by the tests).
        ${CMAKE_SOURCE_DIR}/src/Services/VideoTranscoder.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/Subprocess.cpp
        # Pure waveform helpers (ofs::waveform): bucketing math + cache round-trip, no GL.
        ${CMAKE_SOURCE_DIR}/src/Services/WaveformPeaks.cpp
        ${CMAKE_SOURCE_DIR}/src/Video/PlaybackCursor.cpp
)

target_include_directories(ofs-unit-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)
# Generated StringsGenerated.h (included by Translator.h / test_translator.cpp) lives here.
target_include_directories(ofs-unit-tests SYSTEM PRIVATE ${OFS_GENERATED_DIR})
add_dependencies(ofs-unit-tests ofs_strings_gen)

target_compile_definitions(ofs-unit-tests PRIVATE
        OFS_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        # Resolves getPrefPath() to a temp subdir at compile time so tests never
        # touch the real app data dir. Distinct from the UI test subdir so the two
        # binaries don't clash under parallel ctest.
        OFS_TEST_PREF_SUBDIR="ofs-ng-unit-tests"
        # No data.pak is staged next to the unit-test binary; ofs::res reads themes/translations
        # straight from the source tree instead (Theme.cpp, Translator.cpp).
        OFS_ASSETS_FALLBACK_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(ofs-unit-tests PRIVATE
        ofs-obj-base
        doctest
        imgui
        imnodes
        SDL3::SDL3
        nlohmann_json::nlohmann_json
        glm::glm
        spdlog::spdlog
        concurrentqueue
        thread_pool
        tomlplusplus::tomlplusplus
        miniz
        Bitsery::bitsery
)

ofs_target_pch(ofs-unit-tests)
if (MSVC)
    target_compile_options(ofs-unit-tests PRIVATE /WX /wd4702)
endif ()

set_target_properties(ofs-unit-tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OFS_TEST_BIN_DIR}
)

# ofs-unit-tests links no CoreCLR/libmpv, so it needs only SDL3 staged next to it.
ofs_stage_runtime_dlls(ofs-unit-tests)

# Hook into CTest (enable_testing() is called once at the top-level CMakeLists)
add_test(NAME unit COMMAND ofs-unit-tests)

# --- Plugin-system tests (PluginManager + DotNetHost + NetHost, no window) ---
# Native dispatch/HostApi tests run everywhere; the real CoreCLR load tests REQUIRE the .NET
# runtime and staged managed/ assemblies — a missing runtime fails them rather than skipping.
add_executable(ofs-plugin-tests
        test_main_unit.cpp
        helpers/TestProject.cpp
        plugins/test_plugin_dispatch.cpp
        plugins/test_plugin_load.cpp
        plugins/test_plugin_compat.cpp
        plugins/test_plugin_install.cpp
        plugins/test_script_compile.cpp
        plugins/test_host_api.cpp
        plugins/test_dotnet_host.cpp
        plugins/test_managed_http.cpp
        helpers/PluginManagerTestAccess.cpp

        # Plugin-only src (the shared plain-imgui core comes from ofs-obj-base, linked
        # below). The CoreCLR host + PluginManager are compiled only by this binary
        # (the ui binaries are a different ImGui flavor). PathUtil bakes this binary's
        # OFS_TEST_PREF_SUBDIR value.
        ${CMAKE_SOURCE_DIR}/src/Services/PluginManager.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/ManagedAssemblyTrust.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/ScriptSystem.cpp
        # getFunscriptJson serializes axes through the funscript format helpers.
        ${CMAKE_SOURCE_DIR}/src/Format/Funscript.cpp
        # The interaction-intent routers: the plugin dispatch test drives a plugin edit mode / navigator /
        # selection mode through them (Pass/Drop/Replace, Seek/None), so the plugin binary links them too.
        ${CMAKE_SOURCE_DIR}/src/Services/EditIntentRouter.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/NavigatorRouter.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/SelectIntentRouter.cpp
        ${CMAKE_SOURCE_DIR}/src/Platform/DotNetHost.cpp
        ${CMAKE_SOURCE_DIR}/src/Services/ManagedHttp.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/Http.cpp
        ${CMAKE_SOURCE_DIR}/src/Util/PathUtil.cpp
)
# ManagedHttp's loopback seam test (test_managed_http.cpp) needs the sockets API.
if (WIN32)
    target_link_libraries(ofs-plugin-tests PRIVATE ws2_32)
endif ()

target_include_directories(ofs-plugin-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)
# PluginManager.cpp / ScriptSystem.cpp include the generated OfsBakedHashes.h.
target_include_directories(ofs-plugin-tests SYSTEM PRIVATE ${OFS_GENERATED_DIR})
add_dependencies(ofs-plugin-tests ofs_strings_gen)  # generated StringsGenerated.{h,cpp} for Translator
if (TARGET ofs_baked_hashes)
    add_dependencies(ofs-plugin-tests ofs_baked_hashes)
endif ()

target_compile_definitions(ofs-plugin-tests PRIVATE
        OFS_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        OFS_TEST_PREF_SUBDIR="ofs-ng-plugin-tests"
        # No data.pak is staged next to the plugin-test binary; ofs::res reads the shipped script
        # library (data/scripts/lib/*) straight from the source tree (ScriptSystem compiles a few in
        # test_script_compile.cpp).
        OFS_ASSETS_FALLBACK_DIR="${CMAKE_SOURCE_DIR}"
        # Auto-trusts in PluginManager::isPluginTrusted so the headless loadPlugins() path loads real
        # test plugins with no trust UI. (The addTestPlugin / hostApi seam now rides a friend struct in
        # tests/helpers, no macro needed.)
        OFS_PLUGIN_TEST_HOOKS)

target_link_libraries(ofs-plugin-tests PRIVATE
        ofs-obj-base
        doctest
        imgui
        imnodes
        SDL3::SDL3
        nlohmann_json::nlohmann_json
        glm::glm
        spdlog::spdlog
        concurrentqueue
        thread_pool
        tomlplusplus::tomlplusplus  # Translator.cpp parses lang/*.toml
        miniz
        NetHost::NetHost
)

ofs_target_pch(ofs-plugin-tests)
if (MSVC)
    target_compile_options(ofs-plugin-tests PRIVATE /WX /wd4702)
endif ()

# Own directory (like ui-tests) so its plugins/ and managed/ are isolated.
set_target_properties(ofs-plugin-tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OFS_TEST_BIN_DIR}/plugin-tests
)

# Runtime DLLs the OS resolves next to the executable. NETHOST: plugin-tests links NetHost and hosts
# CoreCLR, so it needs nethost.dll in Debug. (managed/ assemblies are staged separately below, only
# when the .NET SDK is available.)
ofs_stage_runtime_dlls(ofs-plugin-tests NETHOST)

# The real CoreCLR test needs the .NET runtime, the managed/ host assemblies, and the
# built C# test plugin. These are staged when the .NET SDK is available; without it the
# CoreCLR tests fail at runtime (they REQUIRE the runtime — they do not skip).
if (DOTNET_EXECUTABLE AND TARGET Ofs.PluginHost)
    set(TESTPLUGIN_OUT "${OFS_TEST_BIN_DIR}/plugin-tests/plugins/Ofs.TestPlugin")
    add_custom_target(Ofs.TestPlugin ALL
            COMMAND ${DOTNET_EXECUTABLE} build
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/Ofs.TestPlugin/Ofs.TestPlugin.csproj"
            -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${TESTPLUGIN_OUT}"
            -p:PassOutputPathToReferencedProjects=false --nologo -v q
            COMMENT "Building C# test plugin: Ofs.TestPlugin"
            VERBATIM)
    add_dependencies(Ofs.TestPlugin Ofs.Api)

    # A second real plugin used by test_plugin_dispatch.cpp: it encodes every host callback as an
    # observable seek (dispatch/HostApi coverage) and provides the second loaded plugin the
    # persistence test needs (two plugins, different enabled flags).
    set(PROBEPLUGIN_OUT "${OFS_TEST_BIN_DIR}/plugin-tests/plugins/Ofs.ProbePlugin")
    add_custom_target(Ofs.ProbePlugin ALL
            COMMAND ${DOTNET_EXECUTABLE} build
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/Ofs.ProbePlugin/Ofs.ProbePlugin.csproj"
            -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${PROBEPLUGIN_OUT}"
            -p:PassOutputPathToReferencedProjects=false --nologo -v q
            COMMENT "Building C# test plugin: Ofs.ProbePlugin"
            VERBATIM)
    add_dependencies(Ofs.ProbePlugin Ofs.Api)

    add_dependencies(ofs-plugin-tests Ofs.TestPlugin Ofs.ProbePlugin Ofs.PluginHost)
    if (TARGET Ofs.ScriptHost)
        # Ensure managed/Ofs.ScriptHost.dll + Roslyn deps are staged before copy_directory below.
        add_dependencies(ofs-plugin-tests Ofs.ScriptHost)
    endif ()

    # Stage managed/ (Ofs.PluginHost.dll + Ofs.Api.dll + runtimeconfig.json) next to the binary.
    # nethost.dll is staged unconditionally by ofs_stage_runtime_dlls above (plugin-tests always links it).
    # The managed tree now also holds the shipped first-party plugins under managed/plugins; strip that
    # subdir after copying so the base root (getBasePath()/managed/plugins) starts EMPTY and nothing
    # auto-loads at init — these tests drive a controlled plugin set staged into the pref dir instead.
    add_custom_command(TARGET ofs-plugin-tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed" "$<TARGET_FILE_DIR:ofs-plugin-tests>/managed"
            COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:ofs-plugin-tests>/managed/plugins")
else ()
    # No .NET: ensure an empty plugins/ dir exists so loadPlugins() finds nothing.
    add_custom_command(TARGET ofs-plugin-tests PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ofs-plugin-tests>/plugins")
endif ()

# --- Plugin API back-compat fixture (reproduced from the most recent release tag) ---
# A plugin recompiled against the current Ofs.Api can never reveal an API break — it just rebinds to
# whatever surface Ofs.Api now exposes. The only honest witness for the PluginBootstrapper back-compat
# guarantee (same Ofs.Api MAJOR + plugin <= host) is a plugin binary built against an OLDER Ofs.Api and
# NOT recompiled. Rather than commit such a binary, tools/build_compat_plugin.py reproduces one from git:
# it git-archives the most recent release tag's Ofs.Api + Ofs.TestPlugin (selecting the newest tag whose
# Ofs.Api AssemblyVersion MAJOR matches the working tree's) and dotnet-builds them into plugins-compat/.
# test_plugin_compat.cpp then loads that binary against the CURRENT host. The script is non-fatal and
# no-ops cleanly until such a tag exists (mirroring the update checker's no-git-tags gotcha), so this
# block self-skips until a release tag for the current Ofs.Api major exists (now satisfied by v0.1.0).
# find_package(Git) already ran in the root CMakeLists (GIT_EXECUTABLE).
if (DOTNET_EXECUTABLE AND TARGET Ofs.PluginHost AND Python3_FOUND AND GIT_EXECUTABLE)
    set(OFS_COMPAT_PLUGIN_STAGE "${OFS_TEST_BIN_DIR}/plugin-tests/plugins-compat")
    # ALL custom target (like ofs_build_info): re-run every build so a freshly-cut tag is picked up
    # without a reconfigure. The script self-skips via its marker when the selected tag is already built,
    # so steady-state builds only pay a couple of cheap git calls.
    add_custom_target(Ofs.CompatPlugin ALL
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/build_compat_plugin.py"
            --repo "${CMAKE_SOURCE_DIR}"
            --dotnet "${DOTNET_EXECUTABLE}"
            --git "${GIT_EXECUTABLE}"
            --config $<IF:$<CONFIG:Debug>,Debug,Release>
            --out "${OFS_COMPAT_PLUGIN_STAGE}"
            COMMENT "Reproducing back-compat plugin from the latest release tag (build_compat_plugin.py)"
            VERBATIM)
    add_dependencies(ofs-plugin-tests Ofs.CompatPlugin)
    target_compile_definitions(ofs-plugin-tests PRIVATE
            OFS_COMPAT_PLUGIN_DIR="${OFS_COMPAT_PLUGIN_STAGE}")
endif ()

# The API back-compat case lives in its own doctest suite (plugin-compat) and runs as the dedicated
# `plugin-compat` ctest entry below; exclude it here so it isn't run twice.
add_test(NAME plugins COMMAND ofs-plugin-tests --test-suite-exclude=plugin-compat)
# Dedicated entry for the back-compat fixture (own suite). Always registered: it self-skips (passes) when
# no same-major release tag baseline is staged, so CI can track it separately. See test_plugin_compat.cpp.
add_test(NAME plugin-compat COMMAND ofs-plugin-tests --test-suite=plugin-compat)
# Both run from the binary dir so init() finds managed/ and the staged plugins/ + plugins-compat/.
set_tests_properties(plugins plugin-compat PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:ofs-plugin-tests>)

# --- UI automation tests (full window + imgui_test_engine) ---
# Two binaries run the exact same suites against the exact same app, differing only in how they render:
#   ofs-ui-tests          — real GL window. Compiles the whole app (OFS_APP_SOURCES) itself.
#   ofs-ui-tests-headless — OFS_HEADLESS null backend: no GL context, SDL "dummy" video driver, so it
#                           runs on a bare CI box with no display and no Mesa/EGL. Also compiles the app
#                           itself (its OFS_HEADLESS flavor differs from the real-GL binary).
# The suite list and runtime staging (ofs_setup_ui_test_runtime) are factored so the two stay identical.
# OFS_UI_TESTS_GL gates the real-GL binary; the headless one is always built (it is the CI smoke target).
set(OFS_UI_TEST_SUITE_SRC
        test_main_ui.cpp
        helpers/OfsAppTestAccess.cpp
        helpers/PluginManagerTestAccess.cpp
        ui/register_all.cpp
        ui/suite_processing.cpp
        ui/suite_undo.cpp
        ui/suite_dialogs.cpp
        ui/suite_modals.cpp
        ui/suite_project_dialogs.cpp
        ui/suite_playback.cpp
        ui/suite_timeline.cpp
        ui/suite_plugins.cpp
        ui/suite_plugin_ui.cpp
        ui/suite_windows.cpp
        ui/suite_simulator.cpp
        ui/suite_videocontrols.cpp
        ui/suite_command_palette.cpp
        ui/suite_bandbar.cpp
        ui/suite_layouts.cpp
        ui/suite_noproject.cpp
        ui/suite_welcome.cpp
        ui/suite_notifications.cpp
        ui/suite_footer_selectors.cpp
        ui/suite_metadata.cpp
        ui/suite_theme.cpp
        ui/suite_config.cpp
        ui/suite_project_settings.cpp
        ui/suite_menus.cpp
        ui/suite_shortcuts.cpp
        ui/suite_commands.cpp
        ui/suite_multiaxis.cpp
        ui/suite_transcode.cpp
        ui/suite_updates.cpp)

# Link libs shared by both UI test binaries (the imgui_te flavor + the app's third-party deps).
set(OFS_UI_TEST_LIBS
        SDL3::SDL3 imgui_te imgui_stdlib imgui_test_engine glad2 spdlog::spdlog
        nlohmann_json::nlohmann_json glm::glm stb imnodes miniz
        Bitsery::bitsery concurrentqueue thread_pool tomlplusplus::tomlplusplus NetHost::NetHost)

if (OFS_UI_TESTS_GL)
    # Real-GL binary: compiles the suites + the whole app (OFS_APP_SOURCES includes PathUtil.cpp, which
    # bakes this binary's OFS_TEST_PREF_SUBDIR). Same shape as the headless binary below, minus OFS_HEADLESS.
    add_executable(ofs-ui-tests WIN32
            ${OFS_UI_TEST_SUITE_SRC}
            ${OFS_APP_SOURCES})
    target_link_libraries(ofs-ui-tests PRIVATE ${OFS_UI_TEST_LIBS})
    target_include_directories(ofs-ui-tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(ofs-ui-tests SYSTEM PRIVATE
            ${CMAKE_SOURCE_DIR}/lib/libmpv/include ${OFS_GENERATED_DIR})
    add_dependencies(ofs-ui-tests ofs_strings_gen ofs_pack_assets)
    if (TARGET ofs_baked_hashes)
        add_dependencies(ofs-ui-tests ofs_baked_hashes)
    endif ()
    if (WIN32)
        target_link_libraries(ofs-ui-tests PRIVATE dbghelp)
    endif ()
    target_compile_definitions(ofs-ui-tests PRIVATE
            OFS_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            OFS_TEST_PREF_SUBDIR="ofs-ng-ui-tests"
            OFS_PLUGIN_TEST_HOOKS
            OFS_TEST_ENGINE
            OFS_ASSETS_FALLBACK_DIR="${CMAKE_SOURCE_DIR}")
    ofs_target_pch(ofs-ui-tests)
    if (MSVC)
        target_compile_options(ofs-ui-tests PRIVATE /WX /wd4702)
    endif ()
    set_target_properties(ofs-ui-tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${OFS_TEST_BIN_DIR}/ui-tests)
    ofs_setup_ui_test_runtime(ofs-ui-tests)

    add_test(NAME ui-smoke COMMAND ofs-ui-tests)
    # Second pass under a translation (ja_[AI]): suites address widgets by stable ###id, never visible
    # text, so this passes only if every localized label kept a language-independent id.
    add_test(NAME ui-smoke-loc COMMAND ofs-ui-tests "--language=ja_[AI]")
    # LABELS "needs-gl": these open a real GL window. CI runs `ctest -LE needs-gl` (or configures
    # -DOFS_UI_TESTS_GL=OFF) so it never touches them — the headless variant below is the CI smoke test.
    # WORKING_DIRECTORY is the staged binary dir so loadPlugins() scans the staged plugins/ (ctest
    # otherwise runs from the build dir, where it doesn't exist).
    set_tests_properties(ui-smoke ui-smoke-loc PROPERTIES
            WORKING_DIRECTORY $<TARGET_FILE_DIR:ofs-ui-tests>
            LABELS "needs-gl")
endif ()

# Headless null-backend binary — always built; the CI smoke target. Compiles the whole app itself with
# OFS_HEADLESS so ofs::kHeadless is true and every GL path folds away. OFS_APP_SOURCES already includes
# PathUtil.cpp, so this target's OFS_TEST_PREF_SUBDIR bakes correctly without listing it again.
add_executable(ofs-ui-tests-headless WIN32
        ${OFS_UI_TEST_SUITE_SRC}
        ${OFS_APP_SOURCES})
target_link_libraries(ofs-ui-tests-headless PRIVATE ${OFS_UI_TEST_LIBS})
target_include_directories(ofs-ui-tests-headless PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ofs-ui-tests-headless SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/libmpv/include ${OFS_GENERATED_DIR})
add_dependencies(ofs-ui-tests-headless ofs_strings_gen ofs_pack_assets)
if (TARGET ofs_baked_hashes)
    add_dependencies(ofs-ui-tests-headless ofs_baked_hashes)
endif ()
if (WIN32)
    target_link_libraries(ofs-ui-tests-headless PRIVATE dbghelp)
endif ()
target_compile_definitions(ofs-ui-tests-headless PRIVATE
        OFS_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        OFS_TEST_PREF_SUBDIR="ofs-ng-ui-tests-headless"  # own pref dir so it can run beside ofs-ui-tests
        OFS_PLUGIN_TEST_HOOKS
        OFS_TEST_ENGINE
        OFS_HEADLESS  # ofs::kHeadless == true: null GL backend, SDL "dummy" video driver, no display
        OFS_ASSETS_FALLBACK_DIR="${CMAKE_SOURCE_DIR}")
ofs_target_pch(ofs-ui-tests-headless)
if (MSVC)
    target_compile_options(ofs-ui-tests-headless PRIVATE /WX /wd4702)
endif ()
set_target_properties(ofs-ui-tests-headless PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OFS_TEST_BIN_DIR}/ui-tests-headless)
ofs_setup_ui_test_runtime(ofs-ui-tests-headless)

add_test(NAME ui-smoke-headless COMMAND ofs-ui-tests-headless)
add_test(NAME ui-smoke-headless-loc COMMAND ofs-ui-tests-headless "--language=ja_[AI]")
set_tests_properties(ui-smoke-headless ui-smoke-headless-loc PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:ofs-ui-tests-headless>)

# --- Code coverage (Windows / OpenCppCoverage) ---
# Adds a `coverage` target that runs the three ctest binaries under OpenCppCoverage and
# merges their results into one HTML + Cobertura report. Each binary is exported to its own
# binary .cov first (OpenCppCoverage runs a single program per invocation), then merged.
# The target only exists when OpenCppCoverage is on PATH; build it explicitly:
#   cmake --build <build-dir> --target coverage
# Report lands at <build-dir>/coverage/html/index.html.
find_program(OPENCPPCOVERAGE_EXECUTABLE NAMES OpenCppCoverage)
if (WIN32 AND OPENCPPCOVERAGE_EXECUTABLE)
    set(OFS_COV_DIR "${CMAKE_BINARY_DIR}/coverage")
    # OpenCppCoverage rejects '/' separators, so every path it receives must be native ('\').
    # file(TO_NATIVE_PATH) handles the configure-time paths; $<SHELL_PATH:...> handles the
    # generator-expression (target) paths below.
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/src" OFS_COV_SRC)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/lib" OFS_COV_LIB)
    # --modules restricts instrumentation to the test executables; they live under bin/test/.
    file(TO_NATIVE_PATH "${OFS_TEST_BIN_DIR}" OFS_COV_MODULES)
    file(TO_NATIVE_PATH "${OFS_COV_DIR}/unit.cov" OFS_COV_UNIT)
    file(TO_NATIVE_PATH "${OFS_COV_DIR}/plugins.cov" OFS_COV_PLUGINS)
    file(TO_NATIVE_PATH "${OFS_COV_DIR}/ui.cov" OFS_COV_UI)
    file(TO_NATIVE_PATH "${OFS_COV_DIR}/html" OFS_COV_HTML)
    file(TO_NATIVE_PATH "${OFS_COV_DIR}/coverage.xml" OFS_COV_XML)

    # --sources keeps the report to our code; --modules restricts instrumentation to our own
    # binaries so system DLLs aren't profiled (much faster). Two sets of exclusions:
    #   - lib/ : vendored third-party code pulled in via includes.
    #   - imgui_impl : the Dear ImGui SDL3/OpenGL3 backends. These live under src/Platform (so
    #     --sources would otherwise count them) but are vendored verbatim from upstream imgui and
    #     are not code we test. excluded_sources matches on a path substring, so "imgui_impl"
    #     drops imgui_impl_opengl3.cpp, imgui_impl_opengl3_loader.h and imgui_impl_sdl3.cpp.
    set(OFS_COV_FILTERS
            --quiet
            --sources "${OFS_COV_SRC}"
            --excluded_sources "${OFS_COV_LIB}"
            --excluded_sources "imgui_impl"
            --modules "${OFS_COV_MODULES}")

    add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OFS_COV_DIR}"
            COMMAND ${OPENCPPCOVERAGE_EXECUTABLE} ${OFS_COV_FILTERS}
            --export_type "binary:${OFS_COV_UNIT}"
            -- "$<SHELL_PATH:$<TARGET_FILE:ofs-unit-tests>>"
            COMMAND ${OPENCPPCOVERAGE_EXECUTABLE} ${OFS_COV_FILTERS}
            --working_dir "$<SHELL_PATH:$<TARGET_FILE_DIR:ofs-plugin-tests>>"
            --export_type "binary:${OFS_COV_PLUGINS}"
            -- "$<SHELL_PATH:$<TARGET_FILE:ofs-plugin-tests>>"
            COMMAND ${OPENCPPCOVERAGE_EXECUTABLE} ${OFS_COV_FILTERS}
            --working_dir "$<SHELL_PATH:$<TARGET_FILE_DIR:ofs-ui-tests-headless>>"
            --export_type "binary:${OFS_COV_UI}"
            -- "$<SHELL_PATH:$<TARGET_FILE:ofs-ui-tests-headless>>"
            COMMAND ${OPENCPPCOVERAGE_EXECUTABLE} --quiet
            --input_coverage "${OFS_COV_UNIT}"
            --input_coverage "${OFS_COV_PLUGINS}"
            --input_coverage "${OFS_COV_UI}"
            --export_type "html:${OFS_COV_HTML}"
            --export_type "cobertura:${OFS_COV_XML}"
            # The headless UI binary (always built) gives the same suite coverage without needing a GL
            # display, so coverage works regardless of OFS_UI_TESTS_GL.
            DEPENDS ofs-unit-tests ofs-plugin-tests ofs-ui-tests-headless
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "OpenCppCoverage: unit + plugins + ui-smoke-headless -> ${OFS_COV_DIR}/html/index.html"
            USES_TERMINAL VERBATIM)
endif ()

# --- Managed (C#) code coverage (dotnet-coverage) ---
# OpenCppCoverage is a native PDB profiler and cannot see managed IL, so the C# host assemblies
# (Ofs.Api / Ofs.PluginHost / Ofs.ScriptHost) are invisible in the `coverage` report above. This
# `coverage-managed` target runs the two CoreCLR-booting suites (plugins + ui-smoke-headless) under
# dotnet-coverage and merges them into one Cobertura report scoped to those three assemblies (see
# coverage-managed.settings.xml). The native unit suite boots no runtime, so it is not included.
#   dotnet tool install --global dotnet-coverage   # one-time
#   cmake --build <build-dir> --target coverage-managed
# Report lands at <build-dir>/coverage-managed/coverage.cobertura.xml.
# Each suite is launched from its own binary dir (via `cmake -E chdir`) so the staged managed/ host
# assemblies resolve, exactly as ctest runs them.
find_program(DOTNET_COVERAGE_EXECUTABLE NAMES dotnet-coverage)
if (DOTNET_COVERAGE_EXECUTABLE)
    set(OFS_MCOV_DIR "${CMAKE_BINARY_DIR}/coverage-managed")
    set(OFS_MCOV_SETTINGS "${CMAKE_SOURCE_DIR}/tests/coverage-managed.settings.xml")
    add_custom_target(coverage-managed
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OFS_MCOV_DIR}"
            COMMAND ${CMAKE_COMMAND} -E chdir "$<TARGET_FILE_DIR:ofs-plugin-tests>"
            ${DOTNET_COVERAGE_EXECUTABLE} collect --nologo -dco
            --settings "${OFS_MCOV_SETTINGS}"
            --output "${OFS_MCOV_DIR}/plugins.coverage"
            -- "$<TARGET_FILE:ofs-plugin-tests>"
            COMMAND ${CMAKE_COMMAND} -E chdir "$<TARGET_FILE_DIR:ofs-ui-tests-headless>"
            ${DOTNET_COVERAGE_EXECUTABLE} collect --nologo -dco
            --settings "${OFS_MCOV_SETTINGS}"
            --output "${OFS_MCOV_DIR}/ui.coverage"
            -- "$<TARGET_FILE:ofs-ui-tests-headless>"
            COMMAND ${DOTNET_COVERAGE_EXECUTABLE} merge --nologo
            --output "${OFS_MCOV_DIR}/coverage.cobertura.xml"
            --output-format cobertura
            "${OFS_MCOV_DIR}/plugins.coverage" "${OFS_MCOV_DIR}/ui.coverage"
            DEPENDS ofs-plugin-tests ofs-ui-tests-headless
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "dotnet-coverage: managed C# (plugins + ui-smoke-headless) -> ${OFS_MCOV_DIR}/coverage.cobertura.xml"
            USES_TERMINAL VERBATIM)
endif ()

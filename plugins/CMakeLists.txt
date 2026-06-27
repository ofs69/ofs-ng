# DOTNET_EXECUTABLE is discovered once by FindNetHost.cmake (find_package(NetHost REQUIRED) runs in the
# root before this subdir, caching it). Reuse it rather than running a second find_program — the rest of
# the build (tests/) already keys off DOTNET_EXECUTABLE. The guard is defensive: NetHost is REQUIRED, so
# a missing dotnet already fails configure upstream.
if (NOT DOTNET_EXECUTABLE)
    message(WARNING "dotnet not found, C# plugins will not be built.")
    return()
endif()

# Shipped first-party plugins go in bin/managed/plugins/ — inside the managed dir next to the host
# assemblies, NOT a top-level bin/plugins/ that users would mistake for their install folder (see the
# OFS_PLUGIN_OUTPUT_ROOT rationale in the root CMakeLists). Reuse the shared root so the generator and
# the consumer agree on the location.
set(PLUGIN_OUTPUT_ROOT "${OFS_PLUGIN_OUTPUT_ROOT}")

function(add_cs_project PROJECT_NAME PROJECT_PATH OUTPUT_DIR)
    # BYPRODUCTS names the output DLL so the hash-baking command (which DEPENDS on these files) re-runs
    # whenever the DLL's bytes change. Without it the dependency would be on this phony target only —
    # which has no file timestamp — and a managed-source change would leave OfsBakedHashes.h stale,
    # desyncing the baked integrity hash from the DLL on disk.
    add_custom_target(${PROJECT_NAME} ALL
        COMMAND ${DOTNET_EXECUTABLE} build "${PROJECT_PATH}" -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${OUTPUT_DIR}" -p:PassOutputPathToReferencedProjects=false --nologo -v q
        BYPRODUCTS "${OUTPUT_DIR}/${PROJECT_NAME}.dll"
        COMMENT "Building C# project: ${PROJECT_NAME}"
        VERBATIM
    )
endfunction()

# 1. Build the API first (put in managed folder)
add_cs_project(Ofs.Api "${CMAKE_CURRENT_SOURCE_DIR}/Ofs.Api/Ofs.Api.csproj" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed")

# 2. Build the Plugin Host (put in managed folder)
add_cs_project(Ofs.PluginHost "${CMAKE_CURRENT_SOURCE_DIR}/Ofs.PluginHost/Ofs.PluginHost.csproj" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed")
add_dependencies(Ofs.PluginHost Ofs.Api)

# 2b. Build the Script Host (Roslyn-backed inline C#; put in managed folder). Pulls the
# Microsoft.CodeAnalysis NuGet, kept out of the minimal plugin host on purpose.
add_cs_project(Ofs.ScriptHost "${CMAKE_CURRENT_SOURCE_DIR}/Ofs.ScriptHost/Ofs.ScriptHost.csproj" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed")
add_dependencies(Ofs.ScriptHost Ofs.Api)
add_dependencies(Ofs.ScriptHost Ofs.PluginHost)

# 2c. Build Ofs.HostServices (app-internal managed services; put in managed folder). It lives OUTSIDE
# plugins/ on purpose — it references neither Ofs.Api nor any plugin, so it shares no build state with the
# linear Ofs.Api → … chain below and can build concurrently. Built here only because this file already
# owns the managed-build + hash-baking pipeline.
add_cs_project(Ofs.HostServices "${CMAKE_SOURCE_DIR}/Ofs.HostServices/Ofs.HostServices.csproj" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed")

# 3. Build plugins
# Pass DEBUG_ONLY as the optional third argument to keep a plugin out of release
# builds (e.g. sample/demo plugins that must not ship). The target is still
# created in every configuration so dependency edges stay valid, but its build
# command is gated by a generator expression so it only runs in Debug; every
# other config (Release, RelWithDebInfo, MinSizeRel) is treated as a release
# build and skips it. The gate is build-time because the Visual Studio generator
# is multi-config: the config is unknown at configure time.
function(add_cs_plugin PLUGIN_NAME PROJECT_PATH)
    set(PLUGIN_DIR "${PLUGIN_OUTPUT_ROOT}/${PLUGIN_NAME}")

    set(BUILD_CMD ${DOTNET_EXECUTABLE} build "${PROJECT_PATH}" -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${PLUGIN_DIR}" -p:PassOutputPathToReferencedProjects=false --nologo -v q)

    if ("${ARGV2}" STREQUAL "DEBUG_ONLY")
        # Outside Debug the build genex expands to an empty string, leaving an
        # empty COMMAND that CMake omits entirely, so dotnet is never invoked. The
        # second COMMAND only materializes in non-Debug configs and deletes any
        # stale output left behind by a prior Debug build in the shared
        # bin/managed/plugins/ folder, so the plugin can never end up in a release artifact.
        add_custom_target(${PLUGIN_NAME} ALL
            COMMAND "$<$<CONFIG:Debug>:${BUILD_CMD}>"
            COMMAND "$<$<NOT:$<CONFIG:Debug>>:${CMAKE_COMMAND};-E;rm;-rf;${PLUGIN_DIR}>"
            COMMAND_EXPAND_LISTS
            COMMENT "Building C# plugin: ${PLUGIN_NAME} (debug-only, skipped in release builds)"
            VERBATIM
        )
    else()
        # BYPRODUCTS so the hash-baking command re-runs when this DLL changes (see add_cs_project).
        add_custom_target(${PLUGIN_NAME} ALL
            COMMAND ${BUILD_CMD}
            BYPRODUCTS "${PLUGIN_DIR}/${PLUGIN_NAME}.dll"
            COMMENT "Building C# plugin: ${PLUGIN_NAME}"
            VERBATIM
        )
    endif()
    add_dependencies(${PLUGIN_NAME} Ofs.Api)
endfunction()

# SamplePlugin is a user-style sample, NOT a shipped first-party plugin, so it must never live in the
# base plugins dir (bin/managed/plugins) — that root is for first-party plugins only. In Debug it builds
# straight into the user plugins dir (<pref>/plugins = %APPDATA%\ofs\ofs-ng\plugins) so the
# user-installed-plugin path — including the one-time trust prompt — is exercised without
# hand-copying. Outside Debug it isn't built; a stub keeps dependency edges valid where there is no
# known user dir. Either way the base dir is scrubbed of any stale SamplePlugin so it never ships or
# auto-loads from there.
if (WIN32 AND DEFINED ENV{APPDATA})
    file(TO_CMAKE_PATH "$ENV{APPDATA}/ofs/ofs-ng/plugins/SamplePlugin" OFS_SAMPLE_USER_DIR)
    set(SAMPLE_BUILD_CMD ${DOTNET_EXECUTABLE} build "${CMAKE_CURRENT_SOURCE_DIR}/SamplePlugin/SamplePlugin.csproj" -c Debug -o "${OFS_SAMPLE_USER_DIR}" -p:PassOutputPathToReferencedProjects=false --nologo -v q)
    add_custom_target(SamplePlugin ALL
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${PLUGIN_OUTPUT_ROOT}/SamplePlugin"
            COMMAND "$<$<CONFIG:Debug>:${SAMPLE_BUILD_CMD}>"
            COMMAND_EXPAND_LISTS
            COMMENT "SamplePlugin -> user plugins dir (<pref>/plugins, Debug only); base dir kept clean"
            VERBATIM)
else ()
    add_custom_target(SamplePlugin ALL
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${PLUGIN_OUTPUT_ROOT}/SamplePlugin"
            COMMENT "SamplePlugin skipped (no user plugins dir on this platform); base dir kept clean"
            VERBATIM)
endif ()
add_dependencies(SamplePlugin Ofs.Api)

# Every C# target above ProjectReferences Ofs.Api, and each `dotnet build` rebuilds that reference into
# Ofs.Api's own bin/obj (PassOutputPathToReferencedProjects=false keeps the referenced project on its
# default path). The add_dependencies(X Ofs.Api) edges only order X after the *standalone* Ofs.Api
# target — they do NOT stop two dependents from rebuilding the Ofs.Api reference at the same time, which
# makes their GenerateDepsFile tasks collide on Ofs.Api.deps.json ("being used by another process").
# So chain the dependents into one linear order — Ofs.Api → PluginHost → ScriptHost → SamplePlugin →
# Ofs.Core — so no two of them ever run `dotnet build` concurrently. (PluginHost→Api and
# ScriptHost→PluginHost already exist above; these two close the gap.)
add_dependencies(SamplePlugin Ofs.ScriptHost)

add_cs_plugin(Ofs.Core "${CMAKE_CURRENT_SOURCE_DIR}/Ofs.Core/Ofs.Core.csproj")
add_dependencies(Ofs.Core SamplePlugin)

# StarterPlugin is a user-facing TEMPLATE: it is never shipped or deployed in a compiled state (it appears
# in no install/dist path and is not a first-party plugin). We build it here purely as a compile-check so
# template rot — e.g. an Ofs.Api change that breaks the starter — fails our build instead of surfacing only
# when a user copies the template out and builds it. Built to a throwaway dir with packaging (PackPlugin)
# and pref-deploy (DeployToPref) turned off, then scrubbed, so nothing lingers and no artifact is produced.
# OfsDir points the template's Ofs.Api DLL reference at the freshly built managed dir. StarterPlugin uses a
# DLL HintPath (not a ProjectReference) to Ofs.Api, so it never rebuilds Ofs.Api's obj — no race with the
# linear chain above; depending on Ofs.Core just runs it last, after the managed assemblies exist.
set(STARTER_CHECK_DIR "${CMAKE_BINARY_DIR}/startercheck")
add_custom_target(StarterPlugin.Check ALL
    COMMAND ${DOTNET_EXECUTABLE} build "${CMAKE_CURRENT_SOURCE_DIR}/StarterPlugin/StarterPlugin.csproj"
            -c $<IF:$<CONFIG:Debug>,Debug,Release> -o "${STARTER_CHECK_DIR}"
            -p:OfsDir=${CMAKE_RUNTIME_OUTPUT_DIRECTORY} -p:PackPlugin=false -p:DeployToPref=false
            -p:PassOutputPathToReferencedProjects=false --nologo -v q
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${STARTER_CHECK_DIR}"
    COMMENT "Compile-checking StarterPlugin template (not shipped or deployed)"
    VERBATIM)
add_dependencies(StarterPlugin.Check Ofs.Api Ofs.Core)

# Bake the real plugin + managed-assembly hashes once their DLLs are built. DEPENDS on every hashed
# target so the header is regenerated after any of them rebuilds; the script rewrites only on a
# content change, so dependents recompile only when bytes actually changed. The app/tests depend on
# ofs_baked_hashes (src/ and tests/) so this runs before PluginManager.cpp / ScriptSystem.cpp compile.
add_custom_command(
        OUTPUT "${OFS_BAKED_HASHES_HEADER}"
        COMMAND ${CMAKE_COMMAND}
        -DOUT_HEADER=${OFS_BAKED_HASHES_HEADER}
        -DPLUGIN_ROOT=${PLUGIN_OUTPUT_ROOT}
        "-DFIRST_PARTY=${OFS_FIRST_PARTY_PLUGINS}"
        -DMANAGED_ROOT=${OFS_MANAGED_OUTPUT_ROOT}
        "-DMANAGED=${OFS_MANAGED_ASSEMBLIES}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GenBakedHashes.cmake"
        # Depend on the DLL FILES (declared as BYPRODUCTS above), not just the phony build targets, so
        # this re-bakes whenever any DLL's bytes change. Depending on the targets alone left the header
        # cached across a managed-source change, desyncing the baked hash from the DLL on disk.
        DEPENDS
        "${PLUGIN_OUTPUT_ROOT}/Ofs.Core/Ofs.Core.dll"
        "${OFS_MANAGED_OUTPUT_ROOT}/Ofs.Api.dll"
        "${OFS_MANAGED_OUTPUT_ROOT}/Ofs.PluginHost.dll"
        "${OFS_MANAGED_OUTPUT_ROOT}/Ofs.ScriptHost.dll"
        "${OFS_MANAGED_OUTPUT_ROOT}/Ofs.HostServices.dll"
        Ofs.Core Ofs.Api Ofs.PluginHost Ofs.ScriptHost Ofs.HostServices
        COMMENT "Baking plugin + managed-assembly SHA-256 into OfsBakedHashes.h"
        VERBATIM)
add_custom_target(ofs_baked_hashes DEPENDS "${OFS_BAKED_HASHES_HEADER}")

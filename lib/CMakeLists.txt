# = GLAD =
add_library(glad2 STATIC "glad2/src/gl.c")
target_include_directories(glad2 SYSTEM PUBLIC "glad2/include/")

# == GLM ===
add_subdirectory("glm" SYSTEM)

# = STB / cgltf =
# stb_vorbis.c is the full implementation TU (UiSoundService forward-declares the single decode symbol it
# needs rather than including the header, so it's compiled here once). It is third-party and emits a lot
# of MSVC/clang warnings, so silence them on just that file — the app target's /WX never reaches this lib,
# but a clean build log is worth the per-file flag.
add_library(stb STATIC stb/cgltf_impl.c stb/stb_snprintf_impl.c stb/stb_image_write_impl.c stb/stb_vorbis.c)
target_include_directories(stb SYSTEM PUBLIC "stb/")
if (MSVC)
    set_source_files_properties(stb/stb_vorbis.c PROPERTIES COMPILE_OPTIONS "/w")
else ()
    set_source_files_properties(stb/stb_vorbis.c PROPERTIES COMPILE_OPTIONS "-w")
endif ()

# = IMGUI =
# Shared Dear ImGui configuration applied to both the plain `imgui` lib (the shipped app links this) and
# the `imgui_te` test-engine build below; imgui_te additionally enables IMGUI_ENABLE_TEST_ENGINE. Keep
# the two define sets in sync — a mismatch changes sizeof(ImGuiIO) and trips the IMGUI_CHECKVERSION
# struct-layout assert at startup.
set(OFS_IMGUI_DEFINES
    "IMGUI_DEFINE_MATH_OPERATORS="
    "IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1"
    "IMGUI_DISABLE_OBSOLETE_KEYIO=1"
    "IMGUI_USE_STB_SPRINTF=1"
    "IMGUI_DEBUG_PARANOID=1"
)

file (GLOB IMGUI_SOURCES imgui/**.cpp)
file (GLOB IMGUI_HEADERS imgui/**.h)
add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS})
target_include_directories(imgui SYSTEM PUBLIC "imgui/" "stb/")
target_compile_definitions(imgui PUBLIC ${OFS_IMGUI_DEFINES})
target_compile_features(imgui PUBLIC cxx_std_20)
target_link_libraries(imgui PUBLIC stb)

# imgui compiled with test-engine hooks — used by ofs-ui-tests and imgui_test_engine.
# Kept separate so the main app (ofs-ng) can link plain imgui without needing the
# test engine hook implementations.
add_library(imgui_te STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS})
target_include_directories(imgui_te SYSTEM PUBLIC "imgui/" "stb/")
target_compile_definitions(imgui_te PUBLIC ${OFS_IMGUI_DEFINES} "IMGUI_ENABLE_TEST_ENGINE=1")
target_compile_features(imgui_te PUBLIC cxx_std_20)
target_link_libraries(imgui_te PUBLIC stb)

# = IMGUI_STDLIB =
add_library(imgui_stdlib STATIC "imgui/misc/cpp/imgui_stdlib.cpp" "imgui/misc/cpp/imgui_stdlib.h")
target_include_directories(imgui_stdlib SYSTEM PUBLIC "imgui/misc/cpp")
target_link_libraries(imgui_stdlib PUBLIC imgui)

# = SDL3 =
# Configure SDL purely from here (cache vars set before add_subdirectory) — never edit lib/SDL3/
# itself; that vendored tree forbids AI-authored changes (lib/SDL3/CLAUDE.md).
#
# Release statically links SDL3 so the shipped binary carries no SDL3.dll and the linker can strip
# the unused SDL code (/OPT:REF). Debug keeps the shared lib: it relinks faster, and its /MDd CRT
# differs from Release's static /MT (see the root CMakeLists), which a single static SDL couldn't
# satisfy across both. The default follows CMAKE_BUILD_TYPE for single-config generators; OFS_SDL_STATIC
# overrides it explicitly (a multi-config generator leaves CMAKE_BUILD_TYPE empty, so set it there).
if (DEFINED CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_ofs_sdl_static_default ON)
else ()
    set(_ofs_sdl_static_default OFF)
endif ()
option(OFS_SDL_STATIC "Statically link SDL3 (drops the SDL3.dll runtime dependency)" ${_ofs_sdl_static_default})
if (OFS_SDL_STATIC)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
else ()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif ()

# Trim SDL to what OFS uses. We keep Video + Events (always built), the Joystick/HIDAPI stack the
# gamepad input path needs (Window.cpp inits VIDEO|EVENTS|GAMEPAD; OfsApp opens gamepads via HIDAPI for
# non-XInput controllers), Dialog (the native file/folder pickers — ModalManager uses
# SDL_ShowOpen/Save/OpenFolderDialog), and Audio (UiSoundService plays short UI feedback SFX through an
# SDL audio device — video soundtrack still decodes through libmpv). Everything below is unused: the UI
# renders through OpenGL (ImGui_ImplSDL3_InitForOpenGL — never the SDL_Renderer/GPU/Vulkan paths).
# Dropping them shrinks the binary and the build.
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG ON CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)

# Headless CI: the ui-test suite runs entirely on SDL's "dummy" video driver (no window, no GL — see
# the null ImGui backend), so SDL's desktop backends are dead weight there. Building them is the ONLY
# reason CI installs the X11/Wayland/GL -dev packages: the app loads GL at runtime via gl3w's
# dlopen/SDL_GL_GetProcAddress and links SDL as a shared lib, so first-party code needs none of those
# headers. Dropping the backends lets CI shed ~14 apt packages; the shipped build is Windows, so the
# Linux desktop backends were never exercised here anyway. Off by default — only CI sets this.
option(OFS_SDL_HEADLESS "Build SDL with the dummy video driver only (no X11/Wayland/GL — headless CI)" OFF)
if (OFS_SDL_HEADLESS)
    set(SDL_X11 OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_OFFSCREEN OFF CACHE BOOL "" FORCE) # offscreen needs EGL, which we no longer install
    set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    # With no X11/Wayland backend, SDL's configure aborts ("could not create windows") unless we
    # explicitly opt into a console/headless build. That is exactly what we want here.
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
endif ()
add_subdirectory("SDL3/" SYSTEM)

# === json ===
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders ON CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory("json/" SYSTEM)

# === toml++ (header-only; provides tomlplusplus::tomlplusplus) ===
add_subdirectory("tomlplusplus/" SYSTEM)


# Wide-char log filenames so the log path survives a non-ASCII pref dir (e.g. a
# non-ASCII Windows username under %APPDATA%). PUBLIC, so spdlog's compiled lib and
# our TUs agree on spdlog::filename_t (= std::wstring on Windows). See src/Util/Log.cpp.
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "Wide-char log filenames (non-ASCII paths)" FORCE)
add_subdirectory("spdlog/" SYSTEM)

# moodycamel ConcurrentQueue (header-only)
add_library(concurrentqueue INTERFACE)
target_include_directories(concurrentqueue SYSTEM INTERFACE "concurrentqueue/")

# BS::thread_pool (header-only)
add_library(thread_pool INTERFACE)
target_include_directories(thread_pool SYSTEM INTERFACE "thread-pool/include/")

# = MINIZ =
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
add_subdirectory("miniz/" SYSTEM)

# = BITSERY (header-only; provides Bitsery::bitsery) =
# Flat-buffer binary serializer used to pack undo snapshots before miniz-deflating them
# (src/Services/UndoSystem.cpp). Examples/tests stay off; SYSTEM keeps its headers out of /WX.
set(BITSERY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BITSERY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("bitsery/" SYSTEM)

# = IMNODES =
set(IMNODES_IMGUI_TARGET_NAME imgui)
set(IMNODES_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("imnodes/" SYSTEM)
if (MSVC)
    target_compile_options(imnodes PRIVATE /wd4005)
endif ()

# = DOCTEST =
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory("doctest/" SYSTEM)

# = IMGUI TEST ENGINE =
file(GLOB IMGUI_TE_SOURCES "imgui_test_engine/imgui_test_engine/*.cpp")
add_library(imgui_test_engine STATIC ${IMGUI_TE_SOURCES})
target_include_directories(imgui_test_engine SYSTEM PUBLIC
    "imgui_test_engine/imgui_test_engine/"
    "imgui/"
)
# Link imgui PRIVATE so its PUBLIC defines (IMGUI_DISABLE_OBSOLETE_FUNCTIONS etc.)
# propagate into imgui_test_engine source files. Without this the IMGUI_CHECKVERSION()
# call inside imgui_te_engine.cpp sees a different sizeof(ImGuiIO) than imgui.cpp,
# triggering the "Mismatched struct layout!" assert at startup.
# IMGUI_ENABLE_TEST_ENGINE stays PRIVATE: it enables the hook functions inside TE
# sources only; imgui.lib is compiled without it so its hook call-sites expand to
# no-ops and there are no undefined externals in the main (non-test) app.
# PUBLIC: consumers (e.g. test_main_ui.cpp) need the coroutine factory declared in
# imgui_te_coroutine.h, which is gated on IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL.
target_link_libraries(imgui_test_engine PRIVATE imgui_te)
target_compile_definitions(imgui_test_engine
    PRIVATE IMGUI_ENABLE_TEST_ENGINE
    PUBLIC  IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
)
target_compile_features(imgui_test_engine PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(imgui_test_engine PRIVATE /wd4996 /wd4244 /wd4505 /wd4702)
endif()


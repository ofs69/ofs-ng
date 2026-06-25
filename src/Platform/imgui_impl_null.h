// dear imgui: Renderer Backend for a null GL device (headless ui-tests).
// Pairs with the SDL3 platform backend: the SDL3 "dummy" video driver still supplies DisplaySize,
// input and timing from a windowless SDL_Window, while this renderer uploads nothing and submits no
// draw data. It lets the UI test suites run on a bare CI box with no display and no GL/EGL/Mesa.
//
// Mirrors the renderer half of imgui_impl_opengl3 (Init/Shutdown/NewFrame/RenderDrawData) so the app
// can select a renderer backend symmetrically. Unlike upstream's combined imgui_impl_null this is
// renderer-only (we keep SDL3 as the platform backend), and it installs the standard
// DrawCallback_ResetRenderState identifier — which every real backend sets and the app's GL windows
// require, since ImDrawList::AddCallback asserts on a NULL callback.
#pragma once
#include "imgui.h" // IMGUI_IMPL_API
#ifndef IMGUI_DISABLE

IMGUI_IMPL_API bool ImGui_ImplNull_Init();

IMGUI_IMPL_API void ImGui_ImplNull_Shutdown();

IMGUI_IMPL_API void ImGui_ImplNull_NewFrame();

IMGUI_IMPL_API void ImGui_ImplNull_RenderDrawData(ImDrawData *draw_data);

#endif // #ifndef IMGUI_DISABLE

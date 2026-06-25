// dear imgui: Renderer Backend for a null GL device (headless ui-tests). See imgui_impl_null.h.
#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_null.h"

#include <cstdint> // intptr_t

// Draw-callback identifier. The app's GL windows append GetPlatformIO().DrawCallback_ResetRenderState
// to their draw lists; ImDrawList::AddCallback asserts the callback is non-NULL and (via the obsolete
// path) only resolves the sentinel when this field is set, so every renderer backend installs one.
// Intentionally empty: nothing is rendered, so there is no render state to reset.
static void ImGui_ImplNull_DrawCallback_ResetRenderState(const ImDrawList *, const ImDrawCmd *) {}

// Advance one dynamic-atlas texture through its state machine without uploading anything. We advertise
// RendererHasTextures, so ImGui expects the renderer to service these requests. Hand WantCreate a
// non-Invalid dummy id (nothing is bound, but the id must read as valid), acknowledge updates, and
// release on destroy. Mirrors ImGui_ImplOpenGL3_UpdateTexture minus the GL calls.
static void ImGui_ImplNull_UpdateTexture(ImTextureData *tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        tex->SetTexID((ImTextureID)(intptr_t)1); // any id != ImTextureID_Invalid; nothing is uploaded
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

bool ImGui_ImplNull_Init() {
    ImGuiIO &io = ImGui::GetIO();
    IM_ASSERT(io.BackendRendererName == nullptr && "Already initialized a renderer backend!");
    io.BackendRendererName = "ofs_null";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = ImGui_ImplNull_DrawCallback_ResetRenderState;
    return true;
}

void ImGui_ImplNull_Shutdown() {
    ImGuiIO &io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = nullptr;
}

void ImGui_ImplNull_NewFrame() {}

void ImGui_ImplNull_RenderDrawData(ImDrawData *draw_data) {
    // Catch up with texture updates, then drop the draw data: there is no framebuffer to fill, nothing
    // to draw, and no surface to swap. (draw_data->Textures almost always aliases GetPlatformIO().Textures.)
    if (draw_data->Textures != nullptr)
        for (ImTextureData *tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplNull_UpdateTexture(tex);
}

#endif // #ifndef IMGUI_DISABLE

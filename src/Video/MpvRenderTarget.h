#pragma once

#include <cstdint>
#include <mpv/render_gl.h>

// Shared mpv→GL render-target plumbing for the two libmpv players (MpvVideoPlayer and VideoPreview).
// Both render the decoded frame into an offscreen RGBA8 FBO and then sample it as an ImGui texture; the
// FBO setup and the render call are identical, so they live here. Each caller keeps its own sizing
// policy (the main player tracks the requested target size; the preview caps height and keeps aspect).

namespace ofs::mpv {

// Render the current mpv frame into `framebuffer` as a w×h RGBA8 target. No-op if the render context or
// framebuffer is unset or the size is invalid.
void renderToFbo(mpv_render_context *mpvGl, uint32_t framebuffer, int w, int h);

// (Re)allocate `framebuffer` + `frameTexture` as a w×h RGBA8 color attachment, creating the GL objects on
// first use. `label` names the target in the framebuffer-incomplete log line.
void allocRenderTarget(uint32_t &framebuffer, uint32_t &frameTexture, int w, int h, const char *label);

} // namespace ofs::mpv

#include "MpvRenderTarget.h"
#include "MpvLoader.h"
#include "Util/Log.h"
#include <glad/gl.h>

namespace ofs::mpv {

void renderToFbo(mpv_render_context *mpvGl, uint32_t framebuffer, int w, int h) {
    if (!mpvGl || framebuffer == 0 || w <= 0 || h <= 0)
        return;

    mpv_opengl_fbo fbo = {.fbo = static_cast<int>(framebuffer), .w = w, .h = h, .internal_format = GL_RGBA8};
    uint32_t disable = 0;
    mpv_render_param params[] = {{.type = MPV_RENDER_PARAM_OPENGL_FBO, .data = &fbo},
                                 {.type = MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, .data = &disable},
                                 {.type = MPV_RENDER_PARAM_INVALID, .data = nullptr}};
    MpvLoader::mpv_render_context_render(mpvGl, params);
}

void allocRenderTarget(uint32_t &framebuffer, uint32_t &frameTexture, int w, int h, const char *label) {
    if (w <= 0 || h <= 0)
        return;

    if (!framebuffer)
        glGenFramebuffers(1, &framebuffer);
    if (!frameTexture)
        glGenTextures(1, &frameTexture);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glBindTexture(GL_TEXTURE_2D, frameTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frameTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        OFS_CORE_ERROR("{} is not complete: {:x}", label, status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace ofs::mpv

#include "Heatmap.h"

#include "Platform/Headless.h"
#include "Scenegraph/Shader.h"
#include "UI/Theme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glad/gl.h>
#include <memory>

namespace ofs {
class HeatmapShader : public Shader {
  public:
    HeatmapShader() : Shader(vtxShader, fragShader) {
        projMtxLoc = glGetUniformLocation(program, "ProjMtx");
        speedTexLoc = glGetUniformLocation(program, "speedTex");
        colorLutLoc = glGetUniformLocation(program, "colorLut");
        bakeFadeLoc = glGetUniformLocation(program, "bakeFade");
    }

    void setProjMtx(const float *mat4) const { glUniformMatrix4fv(projMtxLoc, 1, GL_FALSE, mat4); }
    void setSpeedTex(int32_t unit) const { glUniform1i(speedTexLoc, unit); }
    void setColorLut(int32_t unit) const { glUniform1i(colorLutLoc, unit); }
    void setBakeFade(bool bake) const { glUniform1f(bakeFadeLoc, bake ? 1.0f : 0.0f); }

  private:
    int32_t projMtxLoc = -1;
    int32_t speedTexLoc = -1;
    int32_t colorLutLoc = -1;
    int32_t bakeFadeLoc = -1;

    static constexpr const char *vtxShader = R"(#version 330 core
        layout (location = 0) in vec2 Position;
        layout (location = 1) in vec2 UV;
        layout (location = 2) in vec4 Color;

        uniform mat4 ProjMtx;

        out vec2 Frag_UV;
        out vec4 Frag_Color;

        void main() {
            Frag_UV = UV;
            Frag_Color = Color;
            gl_Position = ProjMtx * vec4(Position.xy, 0, 1);
        }
    )";

    static constexpr const char *fragShader = R"(#version 330 core
        precision highp float;
        uniform sampler2D speedTex;
        uniform sampler2D colorLut;
        uniform float bakeFade; // 0 = UV.y as alpha (on-screen, blended over the background)
                                // 1 = fade baked into opaque RGB (image export)

        in vec2 Frag_UV;
        in vec4 Frag_Color;
        out vec4 Out_Color;

        void main() {
            float speed = texture(speedTex, vec2(Frag_UV.x, 0.0)).r;
            vec3 color  = texture(colorLut, vec2(speed, 0.5)).rgb;
            // On-screen the vertical fade is alpha (the dark background shows through the
            // top). For an export with no background to blend against, reproduce that look
            // by multiplying the gradient toward black and emitting an opaque pixel.
            Out_Color = mix(vec4(color, Frag_UV.y), vec4(color * Frag_UV.y, 1.0), bakeFade);
        }
    )";
};

static std::unique_ptr<HeatmapShader> sHeatmapShader;
static uint32_t sColorLutTexture = 0;

static constexpr int colorLutResolution = 256;

void Heatmap::rebuildColorLut() {
    if (!sColorLutTexture)
        return;
    std::array<uint8_t, colorLutResolution * 3> lut{};
    const auto &gradient = ofs::theme::getActive().heatmapColors;
    // Gradient position-0 is tuned for line rendering. For the heatmap shader,
    // low-speed areas fade from HeatmapBase (background-matching) instead.
    const ImVec4 &baseVec = ofs::theme::GetStyleColorVec4(AppCol_HeatmapBase);
    for (int i = 0; i < colorLutResolution; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(colorLutResolution - 1);
        float rgb[3];
        gradient.getColorAt(t, rgb);
        float blend = std::min(t / 0.2f, 1.f);
        rgb[0] = baseVec.x + (rgb[0] - baseVec.x) * blend;
        rgb[1] = baseVec.y + (rgb[1] - baseVec.y) * blend;
        rgb[2] = baseVec.z + (rgb[2] - baseVec.z) * blend;
        lut[i * 3 + 0] = static_cast<uint8_t>(std::clamp(rgb[0] * 255.f, 0.f, 255.f));
        lut[i * 3 + 1] = static_cast<uint8_t>(std::clamp(rgb[1] * 255.f, 0.f, 255.f));
        lut[i * 3 + 2] = static_cast<uint8_t>(std::clamp(rgb[2] * 255.f, 0.f, 255.f));
    }
    glBindTexture(GL_TEXTURE_2D, sColorLutTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, colorLutResolution, 1, GL_RGB, GL_UNSIGNED_BYTE, lut.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Heatmap::shutdownShared() {
    if (sColorLutTexture) {
        glDeleteTextures(1, &sColorLutTexture);
        sColorLutTexture = 0;
    }
    sHeatmapShader.reset(); // free the program while the GL context is live, not at static destruction
}

void Heatmap::init() {
    if constexpr (ofs::kHeadless) // null backend: no GL texture/shader; sColorLutTexture stays 0 so
        return;                   // rebuildColorLut() and renderToBitmap() self-skip, draw() never runs
    if (sHeatmapShader)
        return;

    glGenTextures(1, &sColorLutTexture);
    glBindTexture(GL_TEXTURE_2D, sColorLutTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, colorLutResolution, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    rebuildColorLut();

    sHeatmapShader = std::make_unique<HeatmapShader>();
}

Heatmap::Heatmap() {
    init();

    if constexpr (ofs::kHeadless) // m_speedTexture stays 0; ~Heatmap and update() both guard on it
        return;

    glGenTextures(1, &m_speedTexture);
    glBindTexture(GL_TEXTURE_2D, m_speedTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kSpeedResolution, 1, 0, GL_RED, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

Heatmap::~Heatmap() {
    if (m_speedTexture != 0) {
        glDeleteTextures(1, &m_speedTexture);
    }
}

std::vector<float> Heatmap::computeSpeedBuffer(double totalDuration, const VectorSet<ScriptAxisAction> &actions,
                                               float maxSpeed) {
    if (totalDuration <= 0.0)
        return {};

    std::vector<float> speedBuffer(kSpeedResolution, 0.0f);
    std::vector<uint32_t> sampleCountBuffer(kSpeedResolution, 0);

    double timeStep = totalDuration / kSpeedResolution;

    for (size_t i = 0; i + 1 < actions.size(); ++i) {
        const auto &prev = actions[i];
        const auto &next = actions[i + 1];

        double strokeDuration = next.at - prev.at;
        if (strokeDuration <= 0.0)
            continue;

        auto speed = static_cast<float>(std::abs(prev.pos - next.pos) / strokeDuration);

        int prevSampleIdx = static_cast<int>(prev.at / timeStep);
        int nextSampleIdx = static_cast<int>(next.at / timeStep);

        if (prevSampleIdx == nextSampleIdx) {
            if (prevSampleIdx >= 0 && prevSampleIdx < kSpeedResolution) {
                sampleCountBuffer[prevSampleIdx]++;
                speedBuffer[prevSampleIdx] += speed;
            }
        } else {
            int start = std::max(0, prevSampleIdx);
            int end = std::min(kSpeedResolution - 1, nextSampleIdx);
            for (int x = start; x <= end; ++x) {
                sampleCountBuffer[x]++;
                speedBuffer[x] += speed;
            }
        }
    }

    for (int i = 0; i < kSpeedResolution; ++i) {
        if (sampleCountBuffer[i] > 0) {
            speedBuffer[i] /= static_cast<float>(sampleCountBuffer[i]);
        }
        speedBuffer[i] /= maxSpeed;
        speedBuffer[i] = std::clamp(speedBuffer[i], 0.0f, 1.0f);
    }

    return speedBuffer;
}

void Heatmap::update(double totalDuration, const VectorSet<ScriptAxisAction> &actions) const {
    if (m_speedTexture == 0) // no texture to upload into (headless, or GL texture creation failed)
        return;
    const std::vector<float> speedBuffer =
        computeSpeedBuffer(totalDuration, actions, ofs::theme::getActive().heatmapMaxSpeed);
    if (speedBuffer.empty())
        return;

    glBindTexture(GL_TEXTURE_2D, m_speedTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSpeedResolution, 1, GL_RED, GL_FLOAT, speedBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Heatmap::draw(ImDrawList *drawList, const ImVec2 &min, const ImVec2 &max) {
    drawList->AddCallback(
        [](const ImDrawList * /*parentList*/, const ImDrawCmd *cmd) {
            auto *self = static_cast<Heatmap *>(cmd->UserCallbackData);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, sColorLutTexture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, self->m_speedTexture);
            glActiveTexture(GL_TEXTURE0);

            ImDrawData *drawData = ImGui::GetDrawData();
            float l = drawData->DisplayPos.x;
            float r = drawData->DisplayPos.x + drawData->DisplaySize.x;
            float t = drawData->DisplayPos.y;
            float b = drawData->DisplayPos.y + drawData->DisplaySize.y;
            const float orthoProjection[4][4] = {
                {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
                {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
                {0.0f, 0.0f, -1.0f, 0.0f},
                {(r + l) / (l - r), (t + b) / (b - t), 0.0f, 1.0f},
            };
            sHeatmapShader->use();
            sHeatmapShader->setProjMtx(&orthoProjection[0][0]);
            sHeatmapShader->setSpeedTex(1);
            sHeatmapShader->setColorLut(2);
            sHeatmapShader->setBakeFade(false);
        },
        this);

    drawList->AddImage(0, min, max); // Texture ID 0 because we handle it in the callback
    drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
}

std::vector<uint8_t> Heatmap::renderToBitmap(int16_t width, int16_t height, bool fade) const {
    if (width <= 0 || height <= 0 || !sHeatmapShader || !sColorLutTexture || !m_speedTexture)
        return {};

    // Save GL state we'll clobber
    GLint prevFbo = 0, prevProgram = 0, prevVao = 0;
    GLint prevViewport[4] = {};
    GLint prevActiveTex = 0, prevTex0 = 0, prevTex1 = 0, prevTex2 = 0;
    GLboolean blendOn = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);

    // Off-screen FBO with an RGBA8 color attachment
    GLuint fbo = 0, colorTex = 0;
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    std::vector<uint8_t> result;

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glDisable(GL_BLEND);
        glViewport(0, 0, width, height);

        // NDC quad. Without fade, UV.y = 1.0 on all vertices so the gradient is flat and
        // fully saturated. With fade, UV.y runs 0..1 to reproduce the on-screen
        // black→transparent fade (baked into opaque RGB by the shader). The PNG ends up
        // vertically flipped relative to NDC, so NDC y=-1 carries UV.y=0 (black) to land at
        // the top of the image and NDC y=+1 carries UV.y=1 (full gradient) at the bottom.
        const float topV = 1.f;
        const float botV = fade ? 0.f : 1.f;
        struct Vert {
            float pos[2];
            float uv[2];
            float col[4];
        };
        const Vert quad[4] = {
            {.pos = {-1.f, -1.f}, .uv = {0.f, botV}, .col = {}},
            {.pos = {1.f, -1.f}, .uv = {1.f, botV}, .col = {}},
            {.pos = {1.f, 1.f}, .uv = {1.f, topV}, .col = {}},
            {.pos = {-1.f, 1.f}, .uv = {0.f, topV}, .col = {}},
        };
        const GLushort idx[6] = {0, 1, 2, 0, 2, 3};

        GLuint vao = 0, vbo = 0, ebo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        constexpr GLsizei s = sizeof(Vert);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, s, (void *)offsetof(Vert, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, s, (void *)offsetof(Vert, uv));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, s, (void *)offsetof(Vert, col));

        constexpr float kIdentity[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        sHeatmapShader->use();
        sHeatmapShader->setProjMtx(&kIdentity[0][0]);
        sHeatmapShader->setSpeedTex(1);
        sHeatmapShader->setColorLut(2);
        sHeatmapShader->setBakeFade(fade);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sColorLutTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_speedTexture);
        glActiveTexture(GL_TEXTURE0);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

        result.resize(static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());

        glDeleteBuffers(1, &ebo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    }

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glUseProgram(static_cast<GLuint>(prevProgram));
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex2));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex0));
    glActiveTexture(static_cast<GLenum>(prevActiveTex));
    if (blendOn)
        glEnable(GL_BLEND);

    glDeleteTextures(1, &colorTex);
    glDeleteFramebuffers(1, &fbo);

    return result;
}

} // namespace ofs

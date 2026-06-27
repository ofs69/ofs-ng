#include "Shader.h"
#include "Platform/Headless.h"
#include "Util/Log.h"
#include <glad/gl.h>
#include <vector>

namespace ofs {
Shader::Shader(const char *vertexSource, const char *fragmentSource) {
    // Null backend: glad is never loaded, so every gl* entry point is a null pointer. Skip compilation
    // entirely (program stays 0). Shaders are only ever *used* inside dropped ImGui draw callbacks, so
    // a zero program is never bound. SceneShader is a value member of SceneGraph (and so of
    // ScriptSimulator), which is why this ctor runs at startup even with nothing to render.
    if constexpr (ofs::kHeadless)
        return;

    uint32_t vertex = 0, fragment = 0;

    vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vertexSource, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX", false);

    fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fragmentSource, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT", false);

    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    checkCompileErrors(program, "PROGRAM", true);

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::~Shader() {
    if (program != 0) {
        glDeleteProgram(program);
    }
}

void Shader::use() const {
    if (program == 0) // not compiled (headless, or a compile/link failure); only used from draw callbacks
        return;
    glUseProgram(program);
}

void Shader::checkCompileErrors(uint32_t object, const char *label, bool isProgram) {
    int32_t success = 0;
    if (isProgram)
        glGetProgramiv(object, GL_LINK_STATUS, &success);
    else
        glGetShaderiv(object, GL_COMPILE_STATUS, &success);
    if (success)
        return;

    char infoLog[1024];
    if (isProgram)
        glGetProgramInfoLog(object, sizeof(infoLog), nullptr, infoLog);
    else
        glGetShaderInfoLog(object, sizeof(infoLog), nullptr, infoLog);
    OFS_CORE_ERROR("{} of type: {}\n{}\n-- --------------------------------------------------- --",
                   isProgram ? "PROGRAM_LINKING_ERROR" : "SHADER_COMPILATION_ERROR", label, infoLog);
}

// --- VrShader ---

static const char *vrVertexSource = R"(#version 330 core
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

static const char *vrFragBody = R"(
        precision highp float;

        uniform sampler2D Texture;
        uniform vec2 Rotation;
        uniform float Zoom;
        uniform float AspectRatio;
        uniform float VideoAspectRatio;

        in vec2 Frag_UV;
        in vec4 Frag_Color;

        out vec4 Out_Color;

        #define PI 3.1415926535
        #define DEG2RAD 0.01745329251994329576923690768489

        vec3 rotateXY(vec3 p, vec2 angle) {
            vec2 c = cos(angle), s = sin(angle);
            p = vec3(p.x, c.x*p.y + s.x*p.z, -s.x*p.y + c.x*p.z);
            return vec3(c.y*p.x + s.y*p.z, p.y, -s.y*p.x + c.y*p.z);
        }

        float map(float value, float min1, float max1, float min2, float max2) {
            return min2 + (value - min1) * (max2 - min2) / (max1 - min1);
        }

        void main() {
            float inverse_aspect = 1.0 / AspectRatio;
            float hfovRad = hfovDegrees * DEG2RAD;
            float vfovRad = -2.0 * atan(tan(hfovRad/2.0)*inverse_aspect);

            vec2 uv = vec2(Frag_UV.s - 0.5, Frag_UV.t - 0.5);

            vec3 camDir = normalize(vec3(uv.xy * vec2(tan(0.5 * hfovRad), tan(0.5 * vfovRad)), Zoom));
            vec3 camRot = vec3((Rotation - 0.5) * vec2(2.0 * PI, PI), 0.0);

            vec3 rd = normalize(rotateXY(camDir, camRot.yx));

            // acos(rd.y) (not -rd.y): equirect row v=0 is the zenith, so an up-pointing ray must
            // map to the top of the texture. Negating here flips the video vertically.
            vec2 texCoord = vec2(atan(rd.z, rd.x) + PI, acos(rd.y)) / vec2(2.0 * PI, PI);
            if (VideoAspectRatio <= 1.0) {
                texCoord.y = map(texCoord.y, 0.0, 1.0, 0.0, 0.5);
            }
            Out_Color = texture(Texture, texCoord);
        }
    )";

static std::string buildVrFragSource() {
    // {:f} keeps the trailing decimals (e.g. "75.000000") so the value is a valid GLSL float literal.
    return fmt::format("#version 330 core\n        const float hfovDegrees = {:f};\n{}", VrShader::hfovDegrees,
                       vrFragBody);
}

VrShader::VrShader() : Shader(vrVertexSource, buildVrFragSource().c_str()) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    projMtxLoc = glGetUniformLocation(program, "ProjMtx");
    rotationLoc = glGetUniformLocation(program, "Rotation");
    zoomLoc = glGetUniformLocation(program, "Zoom");
    aspectLoc = glGetUniformLocation(program, "AspectRatio");
    videoAspectLoc = glGetUniformLocation(program, "VideoAspectRatio");

    use();
    glUniform1i(glGetUniformLocation(program, "Texture"), 0);
}

void VrShader::setProjMtx(const float *mat4) const {
    glUniformMatrix4fv(projMtxLoc, 1, GL_FALSE, mat4);
}

void VrShader::setRotation(float x, float y) const {
    glUniform2f(rotationLoc, x, y);
}

void VrShader::setZoom(float zoom) const {
    glUniform1f(zoomLoc, zoom);
}

void VrShader::setAspectRatio(float aspect) const {
    glUniform1f(aspectLoc, aspect);
}

void VrShader::setVideoAspectRatio(float aspect) const {
    glUniform1f(videoAspectLoc, aspect);
}

// --- WaveformShader ---

static const char *waveformVertexSource = R"(#version 330 core
        layout (location = 0) in vec2 Position;
        layout (location = 1) in vec2 UV;
        layout (location = 2) in vec4 Color;

        uniform mat4 ProjMtx;

        out vec2 Frag_UV;

        void main() {
            Frag_UV = UV;
            gl_Position = ProjMtx * vec4(Position.xy, 0, 1);
        }
    )";

// The body after the `#version`/constant header, which buildWaveformFragSource() prepends. Keeping the
// body a separate raw-string argument (not the format string) means fmt never tries to parse its GLSL
// braces — the same split buildVrFragSource() uses.
static const char *waveformFragBody = R"(
        precision highp float;

        uniform sampler2D uPeaks; // RG32F, R=min G=max per bucket, row-major
        uniform float uStartBucket; // absolute fractional bucket at the rect's left edge
        uniform float uEndBucket;   // absolute fractional bucket at the rect's right edge
        uniform float uStep;        // ladder group size in buckets (power of two, >= 1)
        uniform float uStride;      // in-group sample stride (>= 1); group covered within kMaxScan samples
        uniform float uBucketsPerPixel; // footprint a single screen pixel spans, for the horizontal supersample
        uniform float uBucketCount;
        uniform float uTexW;
        uniform float uTexH;
        uniform float uScale;
        uniform vec4 uColor;

        in vec2 Frag_UV;
        out vec4 Out_Color;

        // Nearest-texel fetch of one bucket's (min,max), addressing the row-major 2-D layout. NEAREST so
        // a built-in bilinear filter can't blend across row wraps and smear unrelated buckets together.
        vec2 fetchBucket(float idx) {
            idx = clamp(idx, 0.0, uBucketCount - 1.0);
            float row = floor(idx / uTexW);
            float col = idx - row * uTexW;
            vec2 uv = vec2((col + 0.5) / uTexW, (row + 0.5) / uTexH);
            return texture(uPeaks, uv).rg;
        }

        // True (min,max) over ladder group `g`'s buckets [g*uStep, (g+1)*uStep). The group is anchored to
        // absolute bucket 0 and uStep/uStride depend only on the zoom level, so the result is a fixed
        // function of absolute time — never the pan offset. That is what keeps the envelope from boiling
        // (the old shader point-sampled one bucket per fragment against the moving window). Bounded scan:
        // uStride is sized so kMaxScan samples cover the group. Returns (0,0) for a group outside the data.
        vec2 groupMinMax(float g) {
            float g0 = max(g * uStep, 0.0);
            float g1 = min(g * uStep + uStep, uBucketCount);
            float lo = 1.0;
            float hi = -1.0;
            for (int i = 0; i < kMaxScan; ++i) {
                float b = g0 + float(i) * uStride;
                if (b >= g1) break;
                vec2 mm = fetchBucket(b);
                lo = min(lo, mm.x);
                hi = max(hi, mm.y);
            }
            return hi < lo ? vec2(0.0) : vec2(lo, hi);
        }

        // Envelope (min,max) at absolute fractional bucket bAbs. Zoomed out (uStep > 1): true min/max over
        // the anchored ladder group. Zoomed in past the data resolution (uStep == 1): linearly interpolate
        // adjacent buckets so the 1-bucket stairs read as a smooth curve. The interpolation is gated to the
        // zoomed-in case because there it is pure upsampling (no decimation) and so cannot reintroduce the
        // scroll/zoom churn the ladder prevents — it just can't invent detail the data lacks.
        vec2 sampleEnvelope(float bAbs) {
            if (uStep < 1.5) {
                float b0 = floor(bAbs);
                return mix(fetchBucket(b0), fetchBucket(b0 + 1.0), bAbs - b0);
            }
            return groupMinMax(floor(bAbs / uStep));
        }

        // Coverage of vertical position v by an already-resolved [min,max] band, with a >= ~1px feather so
        // the edge is anti-aliased and silence stays a visible centre hairline.
        float bandCoverageMM(vec2 mm, float v, float aa) {
            float mid = (mm.x + mm.y) * 0.5;
            float halfBand = max((mm.y - mm.x) * 0.5, aa);
            float lo = mid - halfBand;
            float hi = mid + halfBand;
            return smoothstep(hi + aa, hi - aa, v) * smoothstep(lo - aa, lo + aa, v);
        }

        // Same, fetching the envelope at fractional bucket bAbs (the zoomed-in interpolation path).
        float bandCoverage(float bAbs, float v, float aa) {
            return bandCoverageMM(sampleEnvelope(bAbs), v, aa);
        }

        void main() {
            // Signal-space vertical coord: +1 top edge, -1 bottom, centered. /uScale (<1) leaves a lane
            // margin so a full-scale peak stops short of the edge. fwidth(v) is just the pixel size in v
            // (v depends only on y), so the feather is identical every frame — no data-derivative pulsing.
            float v = (0.5 - Frag_UV.y) * 2.0 / max(uScale, 0.001);
            float aa = fwidth(v) + 1e-4;

            // The groups are ~1px wide, so point-sampling one per fragment aliases as the waveform scrolls
            // (columns pop up/down). Integrate coverage over the pixel's width instead: sample the envelope
            // at kSuperX fixed sub-positions spanning this pixel's bucket footprint and average. The
            // sub-positions are fixed in screen space and each group's min/max is anchored to absolute
            // buckets, so the average changes smoothly as the view scrolls — the envelope translates as one
            // unit. No height interpolation, so columns never show invented in-between heights.
            const int kSuperX = 8;
            float bCenter = mix(uStartBucket, uEndBucket, Frag_UV.x);

            float cover = 0.0;
            if (uStep < 1.5) {
                // Zoomed in past the data: per-sub interpolation (sampleEnvelope is cheap here, no scan).
                for (int s = 0; s < kSuperX; ++s) {
                    float subOff = (float(s) + 0.5) / float(kSuperX) - 0.5; // -0.5..+0.5 across the pixel
                    cover += bandCoverage(bCenter + subOff * uBucketsPerPixel, v, aa);
                }
            } else {
                // Zoomed out: the pixel's bucket span (uBucketsPerPixel) is <= one ladder group (uStep), so
                // every sub-position lands in one of at most two adjacent groups. groupMinMax is the hot
                // cost (a kMaxScan-iteration min/max scan), so resolve each distinct group ONCE here rather
                // than re-scanning it per sub-position — identical result, up to ~kSuperX/2 fewer scans.
                float halfSpan = 0.5 * uBucketsPerPixel;
                float gL = floor((bCenter - halfSpan) / uStep);
                float gR = floor((bCenter + halfSpan) / uStep);
                vec2 envL = groupMinMax(gL);
                vec2 envR = (gR == gL) ? envL : groupMinMax(gR);
                float boundary = (gL + 1.0) * uStep; // first absolute bucket of the right group
                for (int s = 0; s < kSuperX; ++s) {
                    float subOff = (float(s) + 0.5) / float(kSuperX) - 0.5;
                    float b = bCenter + subOff * uBucketsPerPixel;
                    cover += bandCoverageMM(b < boundary ? envL : envR, v, aa);
                }
            }
            cover /= float(kSuperX);
            Out_Color = vec4(uColor.rgb, uColor.a * cover);
        }
    )";

static std::string buildWaveformFragSource() {
    // Bake kWaveformMaxScan in as a GLSL const so it's a valid (constant) loop bound and stays in sync
    // with the renderer's stride sizing. The body is an argument, so its braces aren't format fields.
    return fmt::format("#version 330 core\n        const int kMaxScan = {};\n{}", kWaveformMaxScan, waveformFragBody);
}

WaveformShader::WaveformShader() : Shader(waveformVertexSource, buildWaveformFragSource().c_str()) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    projMtxLoc = glGetUniformLocation(program, "ProjMtx");
    peaksLoc = glGetUniformLocation(program, "uPeaks");
    startBucketLoc = glGetUniformLocation(program, "uStartBucket");
    endBucketLoc = glGetUniformLocation(program, "uEndBucket");
    stepLoc = glGetUniformLocation(program, "uStep");
    strideLoc = glGetUniformLocation(program, "uStride");
    bppLoc = glGetUniformLocation(program, "uBucketsPerPixel");
    bucketCountLoc = glGetUniformLocation(program, "uBucketCount");
    texWLoc = glGetUniformLocation(program, "uTexW");
    texHLoc = glGetUniformLocation(program, "uTexH");
    scaleLoc = glGetUniformLocation(program, "uScale");
    colorLoc = glGetUniformLocation(program, "uColor");
}

void WaveformShader::setProjMtx(const float *mat4) const {
    glUniformMatrix4fv(projMtxLoc, 1, GL_FALSE, mat4);
}

void WaveformShader::setPeaks(int32_t unit) const {
    glUniform1i(peaksLoc, unit);
}

void WaveformShader::setWindow(float startBucket, float endBucket, float step, float stride,
                               float bucketsPerPixel) const {
    glUniform1f(startBucketLoc, startBucket);
    glUniform1f(endBucketLoc, endBucket);
    glUniform1f(stepLoc, step);
    glUniform1f(strideLoc, stride);
    glUniform1f(bppLoc, bucketsPerPixel);
}

void WaveformShader::setTexDims(float bucketCount, float texW, float texH) const {
    glUniform1f(bucketCountLoc, bucketCount);
    glUniform1f(texWLoc, texW);
    glUniform1f(texHLoc, texH);
}

void WaveformShader::setScale(float scale) const {
    glUniform1f(scaleLoc, scale);
}

void WaveformShader::setColor(float r, float g, float b, float a) const {
    glUniform4f(colorLoc, r, g, b, a);
}
} // namespace ofs

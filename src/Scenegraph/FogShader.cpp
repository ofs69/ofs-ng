#include "FogShader.h"
#include <glad/gl.h>

namespace ofs {

// Shares ImGui's vertex attribute layout (the quad is emitted by ImDrawList::AddImage), so it forwards
// UV exactly like the waveform shader.
static const char *fogVertexSource = R"(#version 330 core
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

static const char *fogFragmentSource = R"(#version 330 core
        precision highp float;

        uniform float uPhase;  // per-frame counter; both translates and evolves the field
        uniform float uAspect; // quad width / height, to keep the noise domain square
        uniform vec4 uColor;   // rgb = themed tint, a = alpha ceiling of a full billow
        uniform vec4 uCenter;  // central vignette: rgb tint, a = peak alpha (toward black or white)

        in vec2 Frag_UV;
        out vec4 Out_Color;

        // Cheap value-noise basis: a hash at integer lattice points, smoothstep-interpolated. Good enough
        // for a soft background; no gradient/simplex cost.
        float hash(vec2 p) {
            p = fract(p * vec2(123.34, 345.45));
            p += dot(p, p + 34.345);
            return fract(p.x * p.y);
        }

        float noise(vec2 p) {
            vec2 i = floor(p);
            vec2 f = fract(p);
            float a = hash(i);
            float b = hash(i + vec2(1.0, 0.0));
            float c = hash(i + vec2(0.0, 1.0));
            float d = hash(i + vec2(1.0, 1.0));
            vec2 u = f * f * (3.0 - 2.0 * f); // smoothstep weights
            return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
        }

        // 4-octave fractal sum. Each octave doubles frequency, halves amplitude — the classic fBm that
        // turns flat noise into billows. Normalized to ~[0,1] by the 0.9375 amplitude sum. Each octave is
        // also rotated by a fixed angle so the layers don't share the value-noise lattice's axis
        // alignment — without it the sum shows faint grid/diagonal streaks.
        float fbm(vec2 p) {
            const mat2 rot = mat2(0.80, 0.60, -0.60, 0.80); // ~37°, irrational-ish so octaves don't realign
            float v = 0.0;
            float a = 0.5;
            for (int i = 0; i < 4; ++i) {
                v += a * noise(p);
                p = rot * p * 2.0;
                a *= 0.5;
            }
            return v / 0.9375;
        }

        // The warped fog density at one UV, before the contrast curve. Factored out of main() so the
        // field can be sampled at both a fragment and its mirror across the vertical centerline.
        float fogDensity(vec2 uv) {
            // Square the domain so the fog cells aren't stretched on wide windows, then scale to a handful
            // of cells across the screen.
            vec2 p = vec2(uv.x * uAspect, uv.y) * 3.0;

            // Time-driven drift (uPhase is advanced by wall-clock seconds, so the speed is identical at any
            // frame rate). It enters here (translation) and again in the warp below at a different
            // rate/direction, so the field both slides and curls/evolves rather than rigidly sliding.
            p += vec2(uPhase * 0.15, uPhase * 0.05);

            // Domain warp: offset the sample point by another fBm lookup. This is what makes the result
            // read as curling fog instead of plain blurred noise.
            vec2 q = vec2(fbm(p), fbm(p + vec2(5.2, 1.3)));
            vec2 r = p + 1.2 * q + vec2(-uPhase * 0.08, uPhase * 0.11);
            return fbm(r);
        }

        void main() {
            // Mirror-average the density across x=0.5 so neither side of the centered welcome content
            // carries more haze than the other — a single drifting field never balances on its own. The
            // average of f(x) and f(1-x) is exactly symmetric, and its horizontal gradient at the
            // centerline is zero, so the two halves meet in a smooth ridge rather than a visible seam.
            // The two fields still drift independently, so the result animates instead of looking frozen.
            float f = 0.5 * (fogDensity(Frag_UV) + fogDensity(vec2(1.0 - Frag_UV.x, Frag_UV.y)));
            // Contrast curve: carve out empty gaps between billows so the haze breathes instead of being
            // a uniform wash. Tighter than a flat wash so the billows pop.
            f = smoothstep(0.30, 0.85, f);

            // Radial position, 0 at the content center and ~1 in the corners (aspect-corrected so it's
            // circular, not elliptical). Centered slightly above the geometric middle to track the welcome
            // screen's content block, which starts ~18% down and grows downward from there.
            vec2 c = (Frag_UV - vec2(0.5, 0.45)) * vec2(uAspect, 1.0);
            float d = length(c) / length(vec2(uAspect, 1.0) * 0.5);

            // Fog pops toward the edges and fades out toward the center, leaving the content area calm.
            float fogA = uColor.a * f * mix(0.2, 1.0, smoothstep(0.15, 0.95, d));
            // A soft vignette tints the center (darker or brighter, per theme), strongest dead-center and
            // trailing off toward the edges. A gaussian falloff rather than a smoothstep: smoothstep has
            // zero slope at both ends, so it builds a flat plateau across the center and ends at a fixed
            // radius — together that reads as a hard-edged disc. The gaussian has neither, so the tint
            // dissolves continuously into the fog with no visible rim.
            float centerA = uCenter.a * exp(-d * d * 3.0);

            // Composite fog (top) over the center vignette (bottom) into one straight-alpha value, so a
            // single quad reproduces exactly what layering the two over the panel would.
            float outA = fogA + centerA * (1.0 - fogA);
            vec3 outRgb = outA > 0.0001 ? (uColor.rgb * fogA + uCenter.rgb * centerA * (1.0 - fogA)) / outA
                                        : vec3(0.0);

            // Dither by ±1/255 to break up banding in these very smooth, low-alpha gradients on an 8-bit
            // framebuffer. A static (gl_FragCoord-locked) hash, so it reads as faint grain, not noise.
            float dith = (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
            Out_Color = vec4(outRgb + dith, outA + dith);
        }
    )";

FogShader::FogShader() : Shader(fogVertexSource, fogFragmentSource) {
    if (program == 0) // base ctor compiled nothing (headless, or a compile failure)
        return;
    projMtxLoc = glGetUniformLocation(program, "ProjMtx");
    phaseLoc = glGetUniformLocation(program, "uPhase");
    aspectLoc = glGetUniformLocation(program, "uAspect");
    colorLoc = glGetUniformLocation(program, "uColor");
    centerLoc = glGetUniformLocation(program, "uCenter");
}

void FogShader::setProjMtx(const float *mat4) const {
    glUniformMatrix4fv(projMtxLoc, 1, GL_FALSE, mat4);
}

void FogShader::setPhase(float phase) const {
    glUniform1f(phaseLoc, phase);
}

void FogShader::setAspect(float aspect) const {
    glUniform1f(aspectLoc, aspect);
}

void FogShader::setColor(float r, float g, float b, float a) const {
    glUniform4f(colorLoc, r, g, b, a);
}

void FogShader::setCenter(float r, float g, float b, float a) const {
    glUniform4f(centerLoc, r, g, b, a);
}
} // namespace ofs

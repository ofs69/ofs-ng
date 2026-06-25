#pragma once

#include <cstdint>
#include <string>

namespace ofs {
class Shader {
  protected:
    uint32_t program = 0;

  public:
    Shader(const char *vertexSource, const char *fragmentSource);

    virtual ~Shader();

    void use() const;

    [[nodiscard]] uint32_t getHandle() const { return program; }

  protected:
    static void checkCompileErrors(uint32_t shader, const std::string &type);
};

class VrShader : public Shader {
  public:
    static constexpr float hfovDegrees = 75.0f;

    VrShader();

    void setProjMtx(const float *mat4) const;

    void setRotation(float x, float y) const;

    void setZoom(float zoom) const;

    void setAspectRatio(float aspect) const;

    void setVideoAspectRatio(float aspect) const;

  private:
    int32_t projMtxLoc = -1;
    int32_t rotationLoc = -1;
    int32_t zoomLoc = -1;
    int32_t aspectLoc = -1;
    int32_t videoAspectLoc = -1;
};

// Upper bound on the per-fragment min/max scan in the waveform shader (see Shader.cpp). The renderer
// sizes the sample stride so a bar's whole bucket range is covered within this many samples, keeping
// CPU and GLSL in agreement.
inline constexpr int kWaveformMaxScan = 32;

// Fills the audio waveform behind the timeline curves. The min/max peak summary is uploaded as an
// RG32F texture (R=min, G=max per bucket). Because a long video needs more buckets than a 1-D texture's
// max width, the buckets are laid out row-major in a 2-D texture (`uTexW` wide, `uTexH` tall).
//
// Temporal stability: the fragment shader does NOT point-sample one bucket per pixel against the moving
// window (that boils as the view scrolls/zooms). The envelope is decimated to min/max over power-of-two
// groups anchored to absolute bucket 0 (the same ladder the timeline's dot decimation uses), so each
// group's value is a fixed function of absolute time. Groups are ~1px wide, so to avoid aliasing that
// 1px-feature signal against the pixel grid as it scrolls (columns popping up/down), each fragment
// supersamples coverage at several fixed sub-positions across its own width and averages — band-limiting
// the motion so the envelope reads as one solid unit translating, not a comb of flickering lines. When
// zoomed in past the data resolution (one bucket spanning many pixels) it instead interpolates adjacent
// buckets, so the stored stairs read as a smooth curve. See WaveformRenderer for the call site.
class WaveformShader : public Shader {
  public:
    WaveformShader();

    void setProjMtx(const float *mat4) const;
    void setPeaks(int32_t unit) const;
    // Visible window as fractional bucket positions at the rect's left/right edges, the decimation group
    // size (`step`, in buckets, a power of two), the in-group sample stride (`stride`, sized so the group
    // is covered within kWaveformMaxScan samples), and `bucketsPerPixel` (the per-pixel footprint the
    // horizontal supersample spans). `step`/`stride` are functions of the zoom level only, so the sampled
    // set stays frame-stable.
    void setWindow(float startBucket, float endBucket, float step, float stride, float bucketsPerPixel) const;
    // Texture geometry needed to turn a bucket index into a texel: total buckets + the 2-D layout dims.
    void setTexDims(float bucketCount, float texW, float texH) const;
    // Fraction of the half-rect height that a full-scale (|sample| == 1) peak reaches.
    void setScale(float scale) const;
    void setColor(float r, float g, float b, float a) const;

  private:
    int32_t projMtxLoc = -1;
    int32_t peaksLoc = -1;
    int32_t startBucketLoc = -1;
    int32_t endBucketLoc = -1;
    int32_t stepLoc = -1;
    int32_t strideLoc = -1;
    int32_t bppLoc = -1;
    int32_t bucketCountLoc = -1;
    int32_t texWLoc = -1;
    int32_t texHLoc = -1;
    int32_t scaleLoc = -1;
    int32_t colorLoc = -1;
};
} // namespace ofs

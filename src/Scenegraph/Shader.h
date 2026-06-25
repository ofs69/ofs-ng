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
} // namespace ofs

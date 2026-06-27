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
    // Log a compile (isProgram=false) or link (isProgram=true) failure. `label` names the stage in the
    // message only — the branch is the explicit bool, not a string compare.
    static void checkCompileErrors(uint32_t object, const char *label, bool isProgram);
};
} // namespace ofs

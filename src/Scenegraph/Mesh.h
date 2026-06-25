#pragma once
#include <cstdint>
#include <span>

namespace ofs::sg {

class Mesh {
    uint32_t vao{};
    uint32_t vbo{};
    uint32_t ebo{};
    int32_t indexCount{};

    static Mesh uploadToGPU(std::span<const float> vertices, std::span<const uint32_t> indices);

  public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;
    Mesh(Mesh &&) noexcept;
    Mesh &operator=(Mesh &&) noexcept;

    [[nodiscard]] static Mesh cube();
    [[nodiscard]] static Mesh fromData(std::span<const float> interleavedVertices, std::span<const uint32_t> indices);
    void draw() const;
};

} // namespace ofs::sg

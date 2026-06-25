#include "Mesh.h"
#include <glad/gl.h>
#include <utility>

namespace ofs::sg {

namespace {

// clang-format off
constexpr float kCubeVertices[] = {
    // position              normal
    // +X face
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f,  0.5f, -0.5f,   1.0f, 0.0f, 0.0f,
     0.5f, -0.5f, -0.5f,   1.0f, 0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
    // -X face
    -0.5f,  0.5f, -0.5f,  -1.0f, 0.0f, 0.0f,
    -0.5f,  0.5f,  0.5f,  -1.0f, 0.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,  -1.0f, 0.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,  -1.0f, 0.0f, 0.0f,
    // +Y face
    -0.5f,  0.5f,  0.5f,   0.0f, 1.0f, 0.0f,
     0.5f,  0.5f,  0.5f,   0.0f, 1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
    // -Y face
    -0.5f, -0.5f, -0.5f,   0.0f,-1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,   0.0f,-1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   0.0f,-1.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,   0.0f,-1.0f, 0.0f,
    // +Z face
    -0.5f, -0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
     0.5f, -0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
    // -Z face
     0.5f, -0.5f, -0.5f,   0.0f, 0.0f,-1.0f,
    -0.5f, -0.5f, -0.5f,   0.0f, 0.0f,-1.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 0.0f,-1.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 0.0f,-1.0f,
};

constexpr uint32_t kCubeIndices[] = {
     0, 1, 2,  0, 2, 3,
     4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11,
    12,13,14, 12,14,15,
    16,17,18, 16,18,19,
    20,21,22, 20,22,23,
};
// clang-format on

} // namespace

Mesh::~Mesh() {
    if (vao != 0) {
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ebo);
    }
}

Mesh::Mesh(Mesh &&o) noexcept
    : vao(std::exchange(o.vao, 0)), vbo(std::exchange(o.vbo, 0)), ebo(std::exchange(o.ebo, 0)),
      indexCount(std::exchange(o.indexCount, 0)) {}

Mesh &Mesh::operator=(Mesh &&o) noexcept {
    if (this != &o) {
        if (vao != 0) {
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vbo);
            glDeleteBuffers(1, &ebo);
        }
        vao = std::exchange(o.vao, 0);
        vbo = std::exchange(o.vbo, 0);
        ebo = std::exchange(o.ebo, 0);
        indexCount = std::exchange(o.indexCount, 0);
    }
    return *this;
}

Mesh Mesh::uploadToGPU(std::span<const float> vertices, std::span<const uint32_t> indices) {
    Mesh m;
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);

    glBindVertexArray(m.vao);

    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size_bytes()), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size_bytes()), indices.data(),
                 GL_STATIC_DRAW);

    constexpr int kStride = 6 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kStride, reinterpret_cast<const void *>(3 * sizeof(float)));

    glBindVertexArray(0);

    m.indexCount = static_cast<int32_t>(indices.size());
    return m;
}

Mesh Mesh::cube() {
    return uploadToGPU(kCubeVertices, kCubeIndices);
}

Mesh Mesh::fromData(std::span<const float> interleavedVertices, std::span<const uint32_t> indices) {
    return uploadToGPU(interleavedVertices, indices);
}

void Mesh::draw() const {
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace ofs::sg

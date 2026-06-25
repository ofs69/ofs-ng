#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ofs::sg {

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{};
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 localMatrix() const {
        auto m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        return glm::scale(m, scale);
    }
};

} // namespace ofs::sg

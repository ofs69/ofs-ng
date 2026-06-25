#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <vector>

namespace ofs {
struct ImGradientMark {
    float color[4];
    float position; // 0 to 1

    ImGradientMark(float r, float g, float b, float a, float pos) {
        color[0] = r;
        color[1] = g;
        color[2] = b;
        color[3] = a;
        position = pos;
    }

    bool operator==(const ImGradientMark &b) const {
        return position == b.position && color[0] == b.color[0] && color[1] == b.color[1] && color[2] == b.color[2] &&
               color[3] == b.color[3];
    }
};

class ImGradient {
  public:
    ImGradient() = default;

    void addMark(float position, const ImColor &color) {
        position = ImClamp(position, 0.0f, 1.0f);
        marks.emplace_back(color.Value.x, color.Value.y, color.Value.z, color.Value.w, position);
        refreshCache();
    }

    void clear() {
        marks.clear();
        refreshCache();
    }

    void getColorAt(float position, float *color) const {
        position = ImClamp(position, 0.0f, 1.0f);
        int cachePos = static_cast<int>(position * 255.0f);
        color[0] = cachedValues[cachePos * 3 + 0];
        color[1] = cachedValues[cachePos * 3 + 1];
        color[2] = cachedValues[cachePos * 3 + 2];
    }

    void refreshCache() {
        std::ranges::sort(marks, {}, &ImGradientMark::position);

        for (int i = 0; i < 256; ++i) {
            computeColorAt(i / 255.0f, &cachedValues[i * 3]);
        }
    }

    const std::vector<ImGradientMark> &getMarks() const { return marks; }
    std::vector<ImGradientMark> &getMarks() { return marks; }

  private:
    void computeColorAt(float position, float *color) const {
        position = ImClamp(position, 0.0f, 1.0f);

        const ImGradientMark *lower = nullptr;
        const ImGradientMark *upper = nullptr;

        for (const auto &mark : marks) {
            if (mark.position < position) {
                if (!lower || lower->position < mark.position) {
                    lower = &mark;
                }
            }
            if (mark.position >= position) {
                if (!upper || upper->position > mark.position) {
                    upper = &mark;
                }
            }
        }

        if (upper && !lower)
            lower = upper;
        else if (!upper && lower)
            upper = lower;
        else if (!lower && !upper) {
            color[0] = color[1] = color[2] = 0;
            return;
        }

        if (upper == lower) {
            color[0] = upper->color[0];
            color[1] = upper->color[1];
            color[2] = upper->color[2];
        } else {
            float distance = upper->position - lower->position;
            float delta = (position - lower->position) / distance;
            color[0] = ImLerp(lower->color[0], upper->color[0], delta);
            color[1] = ImLerp(lower->color[1], upper->color[1], delta);
            color[2] = ImLerp(lower->color[2], upper->color[2], delta);
        }
    }

    std::vector<ImGradientMark> marks;
    float cachedValues[256 * 3] = {0};
};
} // namespace ofs

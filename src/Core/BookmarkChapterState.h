#pragma once
#include "Core/SceneView.h"
#include "imgui.h"
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace ofs {

struct Bookmark {
    double time = 0.0;
    std::string name;
};

struct Chapter {
    double startTime = 0.0;
    double endTime = 0.0;
    std::string name;
    ImU32 color = IM_COL32(70, 130, 180, 220);
    // Remembered scene framing + overlay anchor for this chapter. Absent until the user
    // adjusts the overlay/video while the cursor is inside this chapter (capture-on-adjust).
    std::optional<SceneView> sceneView;
};

struct BookmarkChapterState {
    std::vector<Bookmark> bookmarks;
    std::vector<Chapter> chapters;

    void sortBookmarks() { std::ranges::sort(bookmarks, {}, &Bookmark::time); }

    void sortChapters() { std::ranges::sort(chapters, {}, &Chapter::startTime); }

    // Index of the chapter whose half-open range [startTime, endTime) contains `t`, or -1.
    // chapters is sorted by startTime; chapters may overlap, so this returns the first
    // (earliest-starting) match — matching the band-bar overlap convention.
    [[nodiscard]] int chapterIndexAt(double t) const {
        for (int i = 0; i < static_cast<int>(chapters.size()); ++i) {
            if (t >= chapters[i].startTime && t < chapters[i].endTime)
                return i;
        }
        return -1;
    }
};

} // namespace ofs

#pragma once

#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Heatmap.h"
#include "UI/BandBar.h"
#include <memory>
#include <string>

namespace ofs {

class VideoPlayer;
class VideoPreview;
class TimelinePreviewPopup;
struct AxisModifiedEvent;
struct AxisSelectedEvent;
struct DurationChangedEvent;
struct EvalCompleteEvent;

class VideoControlsWindow {
  public:
    explicit VideoControlsWindow(EventQueue &eventQueue);
    void render(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer, const VideoPreview &preview,
                TimelinePreviewPopup &previewPopup);

  private:
    struct ControlsState {
        float lastPlayerPosition = 0.0f;
        float actualPlaybackSpeed = 1.0f;
        double lastTime = 0.0;
        double speedCalcAccumulator = 0.0;
        bool dragging = false;
        float preMuteVolume = 1.0f;
    };

    struct BookmarkBarState {
        enum class CtxTarget { None, Empty, Bookmark, Chapter };

        CtxTarget activeCtx = CtxTarget::None;
        int ctxIdx = -1;
        double ctxTime = 0.0;
        std::string editName;

        // Which continuous edit in the context popup is mid-gesture (color drag / name typing). The live
        // ModifyEvent rides snapshot=false after the first change, so the whole gesture coalesces into one
        // undo step instead of one per dragged frame / typed character. Reset on menu-open and on the
        // widget's IsItemDeactivated. Mirrors ScriptTimeline's region color gesture.
        enum class ActiveEdit { None, ChapterColor, ChapterName, BookmarkName };
        ActiveEdit activeEdit = ActiveEdit::None;

        struct DragState {
            bool hasDragCandidate = false;
            bool isDragging = false;
            int idx = -1;
            double originalTime = 0.0;
            double previewTime = 0.0;
        } bmDrag;
    };

    void onAxisModified(const AxisModifiedEvent &event);
    void onAxisSelected(const AxisSelectedEvent &event);
    void onDurationChanged(const DurationChangedEvent &event);
    void onEvalComplete(const EvalCompleteEvent &event);

    bool drawTimelineWidget(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer,
                            const VideoPreview &preview, TimelinePreviewPopup &previewPopup, const char *label,
                            float *position);
    void drawBookmarkBar(const ScriptProject &project, EventQueue &eq, VideoPlayer &videoPlayer);

    ControlsState controlsState;
    BookmarkBarState barState;
    ofs::ui::BandBarDragState bandDragState;
    std::shared_ptr<Heatmap> heatmap;
    bool heatmapDirty = true;
    float lastHeatmapMaxSpeed = 0.0f;
    int exportHeight = 64;  // clamped [32, 512] in the export popup
    bool exportFade = true; // bake the on-screen black→transparent fade into the PNG
};

} // namespace ofs

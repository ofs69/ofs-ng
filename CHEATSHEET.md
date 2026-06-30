# Mouse Cheatsheet

A quick reference for the mouse gestures that aren't self-documenting — the ones you
can't discover from a button or menu. Keyboard commands live in the command palette —
open it with **Ctrl+Shift+P** (rebindable) or click the search box in the title bar — and
in the key-binding editor; this page covers the pointer.

**Conventions used below**

- **Click** = press and release without moving. **Drag** = press, move, release.
- **Click–drag empty** starts the gesture on blank space (box-select, pan, scrub);
  **Drag *<thing>*** starts it on that element (a point, band, node…).
- **Alt during a drag** universally *cancels* it — the dragged item snaps back to where it
  started.
- **Dragging a band, chapter, or bookmark snaps to the playhead** when the cursor passes
  near it, so you can park it exactly on the current frame.
- **Middle-click / middle-drag** is reserved for view navigation (pan, reset), never editing.
- **Right-click** opens a context menu almost everywhere — on a point, band, bookmark, node,
  the video, or empty space. The right-click rows below note only what each menu is *for*.

**Terms**

- **Point** — a single action (keyframe) on an axis: a time + value.
- **Strip** — the axis list down the left side of the timeline.
- **Band** — a Processing Region or chapter block in a bar; drag its body to move, its edge to resize.
- **Grip** — a drag handle on the simulator overlay (center grip to move, corner grip to resize).
- **Playhead** — the current-frame cursor; drags snap to it (see above).
- **Group / lead** — an edit group spans several axes at once; the *lead* is the one edits originate from.

---

## Timeline

> **Interaction modes.** Script Timeline editing, selection, and stepping all route through
> swappable *modes* chosen in the timeline footer:
>
> | Footer selector | Governs | Default |
> |-----------------|---------|---------|
> | **Edit mode** | What a point edit does (add / move / remove) | *Native* |
> | **Select** | Which points a gesture selects | *Native* |
> | **Overlay** | The grid drawn over the script line (frames / tempo beats) | — |
> | **Step** | Where the playhead lands when stepped | *Follow overlay* |
>
> The gestures in **Script Timeline** below describe the *Native* edit/select defaults. A
> plugin mode can reinterpret the same gestures (e.g. a stamp or ruler tool), and a ▾/gear
> affordance next to an active mode opens its options.

### Strip (axis list, left side)

| Gesture | Action |
|---------|--------|
| Click | Select axis |
| Ctrl+click | Add axis to / remove from edit group |
| Click existing group member | Promote to group lead |
| Click–drag across rows | Build group from spanned axes |
| Double-click | Select axis, dissolve group |
| Right-click | Axis context menu |

### Script Timeline

| Gesture | Action |
|---------|--------|
| Click empty | Seek |
| Shift+click empty | Add point |
| Click–drag empty | Box-select |
| Ctrl+drag empty | Add to selection |
| Click point | Select point + seek |
| Ctrl+click point | Toggle point selection |
| Drag point | Move (time + value) |
| Alt+drag point | Move value only (locks time) |
| Middle-drag | Pan |
| Middle double-click | Clear selection |
| Scroll | Zoom |
| Right-click | Context menu |

The script-line layout (right-click → *Stacked lines* / *Separate lanes*) sets whether every axis shares one
band (Overlay) or each gets its own row (Lanes). In **Lanes**, edit gestures still target the active axis
and fan across its edit group — only the geometry differs (a gesture maps to the active axis's lane):

| Gesture (Lanes only) | Action |
|---------|--------|
| Click an inactive lane | Focus that axis (select / group-lead / Ctrl-toggle, like its strip row) — no seek |
| Shift+scroll | Scroll the lanes vertically (when more axes than fit; a scrollbar appears) |
| Drag scrollbar (right edge) | Scroll the lanes vertically |

### Processing Region bar

| Gesture | Action |
|---------|--------|
| Click band | Select |
| Drag edge | Resize |
| Drag body | Move |
| Alt+drag | Cancel drag, snap back |
| Right-click band | Context menu |
| Right-click empty | Add Processing Region |

---

## Controls bar

### Heatmap seek bar

| Gesture | Action |
|---------|--------|
| Click | Seek |
| Click–drag | Scrub |
| Right-click | Export heatmap |

### Bookmark / chapter bar

| Gesture | Action |
|---------|--------|
| Click bookmark | Seek |
| Drag bookmark | Move bookmark |
| Alt+drag bookmark | Cancel, snap back |
| Right-click bookmark | Edit / delete |
| Drag chapter edge | Resize chapter |
| Drag chapter body | Move chapter |
| Alt+drag chapter | Cancel, snap back |
| Right-click chapter | Edit / delete |
| Right-click empty | Add bookmark / chapter |

---

## Video player

| Gesture | 2D mode | VR mode |
|---------|---------|---------|
| Drag | Pan | Rotate (yaw + pitch) |
| Middle double-click | Reset pan & zoom + recenter overlay | Reset view + recenter overlay |
| Scroll | Zoom | Zoom |
| Right-click | Context menu (incl. *Reset Video View*) | Context menu (incl. *Reset Video View*) |

The middle double-click also has command-palette / rebindable forms: **Reset Video View**, **Reset
Simulator Overlay**, and **Reset View & Overlay** (the combined gesture). A locked view/overlay ignores
its reset, the same as a drag.

---

## Simulator

### 3D overlay / 2D bar

| Gesture | Action |
|---------|--------|
| Shift+hover | Preview axis value at cursor |
| Shift+click | Place action at previewed value |
| Drag center grip | Move overlay |
| Drag resize grip (bottom-right) | Resize overlay |
| Drag bar endpoint (2D) | Adjust range |
| Alt+drag | Cancel drag, snap back |
| Middle double-click (on video) | Recenter overlay |
| Right-click | Context menu (lock, 2D/3D, invert, *Reset Simulator Overlay*, …) |

Recentering is also reachable as the **Reset Simulator Overlay** command (palette / rebindable); a
locked overlay ignores it.

---

## Processing panel (node graph)

| Gesture | Action |
|---------|--------|
| Click node | Select |
| Drag node | Move |
| Drag output pin | Create link |
| Drag numeric field | Adjust value |
| Right-click empty | Add node |
| Right-click node | Context menu (Duplicate / Disconnect / Delete) |
| Right-click link | Context menu (Delete) |
| `F` | Fit view to the selection (or the whole graph) — also the header **Fit** button |

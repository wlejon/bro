// Shell-drawn window frames: when bro is the display server, the shell can
// decorate a client window with an element of its own document,
//
//     <div data-window-frame="42"> title bar, buttons, edges... </div>
//
// and the engine keeps that element on the window: it sizes and places it
// from the window's frame plus the decoration insets the shell declared
// (bro.compositor.setDecorations), paints it just below its window in the
// window stacking order (so the window above covers it, and it covers the
// windows below), and routes the pointer over it to the shell. The shell
// never positions a frame itself, so a frame follows its window in the same
// frame the window moves, with no script in between.
//
// What the engine writes on a frame element (inline style and attributes; the
// shell styles everything else):
//   position: fixed, box-sizing: border-box, left / top / width / height (the
//   outer rect), z-index (1 + its window's place in the stack), display: none
//   while its window is not shown with a frame (no such window, minimized,
//   fullscreen, on another workspace, not decorated, no insets);
//   data-window-state="normal" | "maximized", data-window-snap="left" | ...
//   (absent when not snapped), data-window-focused (present on the focused
//   window's frame).
#pragma once

#include "dom/node_handle.h"
#include "layout/draw_traversal.h"
#include "render/layer_source.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::dom {
class Document;
class Element;
}

namespace bro::engine {

// A shown client window as the frames see it (bottom to top in a stack).
struct FrameWindow {
    uint64_t id = 0;
    int x = 0, y = 0, width = 0, height = 0;                    // the client
    int insetLeft = 0, insetTop = 0, insetRight = 0, insetBottom = 0;  // the frame's reach around it
    bool focused = false;
    bool maximized = false;
    std::string snap = "none";

    bool framed() const { return insetLeft > 0 || insetTop > 0 || insetRight > 0 || insetBottom > 0; }
};

class WindowFrames {
public:
    // Takes the window stack (bottom to top) and brings every frame element
    // of `doc` up to date with it. Writes only what changed, so a still
    // desktop costs no restyle.
    void sync(dom::Document* doc, std::vector<FrameWindow> stack);

    const std::vector<FrameWindow>& stack() const { return stack_; }
    // The frame element drawn for a window (null: none shown).
    dom::Element* frameOf(uint64_t windowId) const;
    // The stack with each window's frame element, for the paint pass.
    std::vector<layout::DrawTraversal::ClientWindowSlot> slots() const;

    // Client-window runs recorded by paint passes, kept for the frames that
    // composite them later (render::ClientWindowsLayerSource::list).
    std::vector<render::ClientWindowRef>& beginList(uint32_t& id);
    const std::vector<render::ClientWindowRef>* list(uint32_t id) const;

private:
    struct Written {
        std::string left, top, width, height, z;
        bool hidden = false;
        bool placed = false;
        std::string state, snap;
        bool focused = false;
    };
    // Every [data-window-frame] element (document order) with the window it
    // names and what was last written on it. Handles, not pointers: a frame
    // the shell removed may be freed before the next rescan.
    struct Entry {
        dom::ElementHandle element;
        uint64_t window = 0;
        Written written;
    };
    void rescan(dom::Document* doc);
    void write(Entry& e, dom::Element* el, const FrameWindow* w, int z);

    dom::Document* doc_ = nullptr;
    uint64_t scannedEpoch_ = ~uint64_t(0);
    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, dom::ElementHandle> shown_;  // window id -> the frame drawn for it
    std::vector<FrameWindow> stack_;

    uint32_t nextList_ = 1;
    std::deque<std::pair<uint32_t, std::vector<render::ClientWindowRef>>> lists_;
};

}  // namespace bro::engine

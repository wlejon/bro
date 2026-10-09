// The OS-neutral half of drags out of an SDL window: the queued drag, the
// reports, and the stale button release. See sdl_drag.h.
#include "platform/sdl/sdl_drag.h"
#include "platform/event_loop.h"

#include <SDL3/SDL.h>

#include <optional>
#include <utility>
#include <vector>

namespace bro::platform {

namespace {

struct PendingDrag {
    SDL_Window* window = nullptr;
    uint32_t windowId = 0;
    DragSource drag;
};

EventLoop* g_loop = nullptr;
std::optional<PendingDrag> g_pending;
bool g_active = false;
uint8_t g_staleButton = 0;  // whose release SDL still owes; 0 none
std::vector<DragReport> g_reports;
bool g_delivering = false;

}  // namespace

void sdlDragAttach(EventLoop* loop) { g_loop = loop; }

bool sdlQueueDrag(SDL_Window* window, uint32_t windowId, const DragSource& drag) {
    if (!window || drag.data.empty() || !sdlNativeDragSupported()) return false;
    if (g_active || g_pending) return false;
    g_pending = PendingDrag{window, windowId, drag};
    return true;
}

void sdlRunPendingDrag() {
    if (!g_pending) return;
    PendingDrag p = std::move(*g_pending);
    g_pending.reset();
    // The pointer events SDL queued behind the one that began the drag are
    // the drag's now: the OS reports where it goes.
    SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_UP);
    g_active = true;
    if (!sdlNativeDragBegin(p.window, p.windowId, p.drag)) {
        g_active = false;
        DragReport end;
        end.action = "none";
        sdlDragReport(std::move(end));
    }
    sdlDeliverDragReports();
}

bool sdlOwnDragActive() { return g_active; }

void sdlDragReport(DragReport report) { g_reports.push_back(std::move(report)); }

void sdlDragFinished(uint8_t button) {
    g_active = false;
    g_staleButton = button;
}

bool sdlTakeStaleButtonUp(uint8_t button) {
    if (!g_staleButton || button != g_staleButton) return false;
    g_staleButton = 0;
    return true;
}

void sdlButtonPressed(uint8_t button) {
    if (button == g_staleButton) g_staleButton = 0;
}

void sdlDeliverDragReports() {
    if (!g_loop || g_delivering || g_reports.empty()) return;
    g_delivering = true;
    std::vector<DragReport> reports;
    reports.swap(g_reports);
    EventLoop& loop = *g_loop;
    for (const DragReport& r : reports) {
        switch (r.kind) {
            case DragReport::Kind::Motion:
                if (loop.onOwnDragMotion) loop.onOwnDragMotion(r.windowId, r.x, r.y);
                break;
            case DragReport::Kind::Leave:
                if (loop.onOwnDragLeave) loop.onOwnDragLeave(r.windowId);
                break;
            case DragReport::Kind::Drop:
                if (loop.onOwnDragDrop) loop.onOwnDragDrop(r.windowId, r.x, r.y);
                break;
            case DragReport::Kind::End:
                if (loop.onOwnDragEnd) loop.onOwnDragEnd(r.action);
                break;
        }
    }
    g_delivering = false;
}

#if !defined(_WIN32) && !defined(__APPLE__)
// SDL's X11 / Wayland backends: no drag source (Linux desktops run bro's own
// Wayland backend, which has one).
bool sdlNativeDragSupported() { return false; }
bool sdlNativeDragBegin(SDL_Window*, uint32_t, const DragSource&) { return false; }
#endif

}  // namespace bro::platform

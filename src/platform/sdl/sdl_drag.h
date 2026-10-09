#pragma once
// Drags out of an SDL window. SDL has no drag-source API, so the OS's own is
// driven beside it: OLE DoDragDrop on Windows (sdl_drag_win.cpp), an
// NSDraggingSession on macOS (sdl_drag_mac.mm). SDL stays the window's owner
// and its drop target; this only adds the source side.
//
// SdlWindow::startDrag queues the drag (sdlQueueDrag); the event loop starts
// it right after the event whose handler asked for it (sdlRunPendingDrag), so
// the OS drag never begins inside the engine's own event dispatch. What the
// drag does is reported back as DragReports, which the event loop hands to
// its onOwnDrag* handlers (sdlDeliverDragReports). Internal to src/platform.

#include "platform/window.h"

#include <cstdint>
#include <string>

struct SDL_Window;

namespace bro::platform {

class EventLoop;

/// The event loop the drag reports go to (null to detach).
void sdlDragAttach(EventLoop* loop);

/// Queue `drag` to leave `window`. False where the OS drag is unavailable
/// (Linux under SDL, a drag already under way, nothing to carry).
bool sdlQueueDrag(SDL_Window* window, uint32_t windowId, const DragSource& drag);

/// Start the queued drag, if there is one. On Windows this runs the whole
/// drag (DoDragDrop is modal); on macOS it begins the session and returns.
void sdlRunPendingDrag();

/// A drag the OS carries is under way: the pointer events SDL reports
/// meanwhile are not the page's (macOS, where the session is not modal).
bool sdlOwnDragActive();

/// Hand the queued reports to the attached event loop's onOwnDrag* handlers.
void sdlDeliverDragReports();

/// The OS drag swallowed the button release that ended it, so SDL reports it
/// late, or synthesises it when it resyncs its button state: true (once) for
/// that release, which the event loop then drops. A press of the button
/// forgets it.
bool sdlTakeStaleButtonUp(uint8_t button);
void sdlButtonPressed(uint8_t button);

// --- For the per-OS halves ---

struct DragReport {
    enum class Kind { Motion, Leave, Drop, End } kind = Kind::End;
    uint32_t windowId = 0;
    float x = 0, y = 0;  // window coordinates (Motion, Drop)
    std::string action;  // End: "copy" / "move" / "link" / "none"
};

/// Queue a report for sdlDeliverDragReports.
void sdlDragReport(DragReport report);
/// The drag is over: no longer active, and SDL still owes the release of
/// `button` (sdlTakeStaleButtonUp). Queues no report.
void sdlDragFinished(uint8_t button);

/// The OS half: whether it exists, and start (Windows: run) one drag.
/// False when it could not begin; the caller then ends the drag as refused.
bool sdlNativeDragSupported();
bool sdlNativeDragBegin(SDL_Window* window, uint32_t windowId, const DragSource& drag);

}  // namespace bro::platform

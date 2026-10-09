#pragma once

// What the engine asks of whoever drives <remoteview> elements: the remote
// viewer (bronze_host/host_remote_view.cpp, in a build with
// BRO_WITH_REMOTE). The engine knows the element (layout::ElRemoteView) and
// where its layer goes; the host owns the session, its pictures and the wire.
// Main thread throughout.
//
// Input: while a view is captured (layout::ElRemoteView::captured, and the
// element focused), every key goes to the host before anything of bro's own
// sees it (no DOM event, no hotkey, no text input): the remote screen gets
// Alt+Tab and Ctrl+W as a local one would. Pointer input goes to the host
// while it is over a view (or a button pressed there is held), captured or
// not; a press focuses and captures the view.

#include "render/layer_image.h"

#include <cstdint>

namespace bro::layout { class ElRemoteView; }

namespace bro::engine {

class RemoteViewHost {
public:
    virtual ~RemoteViewHost() = default;

    /// The picture a view shows (SHADER_READ_ONLY_OPTIMAL), or an empty
    /// image when it has none. `serial` changes whenever the picture does
    /// (the frame's key).
    virtual render::LayerImage layerImage(uint64_t viewId, uint64_t& serial) = 0;
    /// The frame that showed these views' pictures was presented now.
    virtual void presented(const uint64_t* viewIds, size_t count) = 0;

    /// A key, by platform::Scancode. True when the host took it (always,
    /// while the view is captured).
    virtual bool key(layout::ElRemoteView& view, int scancode, int mod, bool pressed, bool repeat) = 0;
    /// The pointer over a view: (x, y) in CSS px relative to the view's
    /// content box (w x h). `button` 0 for motion, else the engine's mouse
    /// button number (1 left, 2 middle, 3 right, 4 and 5 the side buttons)
    /// with `pressed`. (xrel, yrel): the device's movement, CSS px.
    virtual void pointer(layout::ElRemoteView& view, float x, float y, float w, float h, float xrel, float yrel,
                         int button, bool pressed) = 0;
    /// The wheel over a view, in detents (+y away from the user).
    virtual void wheel(layout::ElRemoteView& view, float dx, float dy) = 0;
    /// The view lost the keyboard (blur, the window lost focus, the release
    /// chord): let go of every key and button held through it.
    virtual void releaseAll(layout::ElRemoteView& view) = 0;
    /// Whether the remote screen's pointer is locked: the local pointer is
    /// then hidden and held, and only its movement is sent.
    virtual bool pointerLocked(layout::ElRemoteView& view) = 0;
};

}  // namespace bro::engine

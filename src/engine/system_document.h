#pragma once

#include "engine/mouse_dispatch_state.h"
#include "engine/sub_doc_animations.h"
#include "dom/document.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::canvas { class CanvasScene; }

namespace bro::engine {

/// System panels are ordinary HTML documents rendered through the same
/// layout/raster pipeline as the app document. They share the engine's
/// textMetrics_ for layout, and in windowed mode the raster thread draws
/// each visible panel into its own GPU surface.
///
/// A panel is found at startup but built (parsed, laid out, its scripts run)
/// only the first time it is shown: most windows never open the inspector or
/// the settings, and building all of them cost every app ~250 ms before its
/// first frame. `document` stays null until then.
struct SystemDocument {
    // Out of line (system_panels.cpp), where CanvasScene is complete.
    SystemDocument();
    ~SystemDocument();
    SystemDocument(SystemDocument&&) noexcept;
    SystemDocument& operator=(SystemDocument&&) noexcept;

    std::string name;
    std::string tabLabel;
    std::string group;
    std::string htmlPath;   // the panel's .html
    std::string dirPath;    // its directory (scripts and assets resolve here)
    bool loaded = false;    // built (or tried to be: an unreadable file stays unloaded)
    bool active = true;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
    // Its CSS transitions, animations and Web Animations, built with it.
    // Before it: they outlive it (the document's teardown reaches them), and
    // are cleared first (their records name its elements).
    std::unique_ptr<SubDocAnimations> animations;
    std::unique_ptr<dom::Document> document;
    MouseDispatchState mouseState;  // per-doc click/dblclick tracking
};

} // namespace bro::engine

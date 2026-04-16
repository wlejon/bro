#pragma once

#include "render/renderer.h"
#include <memory>
#include <string>

namespace bro::engine {

/// Rendering context an overlay belongs to.
/// Overlays are drawn at the end of their context's raster pass so they
/// render on top of everything else in that context.
enum class OverlayContext {
    App,    // drawn by the raster thread over the app document
    System, // drawn by the system renderer over system panels
};

/// Base class for an engine-level overlay (dropdown, color picker, etc.).
///
/// Overlays are screen-space widgets that sit on top of the DOM tree and
/// own hit-testing for their bounds. The OverlayManager dispatches input
/// events to the active overlay before the DOM sees them, so hover/click
/// can never leak through to elements underneath.
///
/// Coordinates are all in screen space — overlays do not scroll with
/// document content.
class Overlay {
public:
    virtual ~Overlay() = default;

    /// Screen-space bounds used by the manager to classify inside/outside
    /// clicks. Mouse events inside these bounds are always consumed;
    /// clicks outside dismiss the overlay by default.
    virtual void getBounds(float& x, float& y, float& w, float& h) const = 0;

    /// Render the overlay using the given renderer. Caller has already
    /// called save() + resetClip(); the overlay must call restore() itself
    /// if it changes clip/transform, but usually just draws in screen space.
    virtual void draw(render::Renderer* r) = 0;

    // --- Input ---
    // All coords are screen space. Returning true consumes the event.

    virtual bool onMouseDown(float x, float y, int button);
    virtual bool onMouseMove(float x, float y);
    virtual bool onMouseUp(float x, float y, int button);
    virtual bool onWheel(float x, float y, float dx, float dy);
    virtual bool onKeyDown(int keycode, int mod);
    virtual bool onTextInput(const std::string& text);

    /// Called once by the manager just before destroying the overlay.
    /// Overrides can fire final callbacks, restore state, etc.
    virtual void onDismiss() {}

    /// If true, clicking outside the bounds dismisses the overlay.
    virtual bool dismissesOnClickOutside() const { return true; }

    /// If true, pressing Escape dismisses the overlay.
    virtual bool dismissesOnEscape() const { return true; }

    /// The overlay should request dismissal from inside its own logic
    /// (e.g. after the user picks an option). The manager checks this
    /// after each event dispatch.
    bool dismissRequested() const { return dismissRequested_; }
    void requestDismiss() { dismissRequested_ = true; }

    OverlayContext context() const { return context_; }
    void setContext(OverlayContext ctx) { context_ = ctx; }

    /// If the overlay needs a font, it can lazily create one through the
    /// renderer. The renderer pointer is provided at draw time; overlays
    /// that need to measure text before drawing can use measureRenderer_
    /// once set by the manager on open.
    void setMeasureRenderer(render::Renderer* r) { measureRenderer_ = r; }

    /// True if (x, y) lies within getBounds().
    bool pointInBounds(float x, float y) const;

protected:
    render::Renderer* measureRenderer_ = nullptr;

private:
    bool dismissRequested_ = false;
    OverlayContext context_ = OverlayContext::App;
};

/// Manages the single currently-active overlay. Input handlers in Engine
/// dispatch to the manager before consulting the DOM. Draw calls from the
/// raster pass (app) and system panel pass invoke drawIfContext() with
/// their own renderer.
class OverlayManager {
public:
    /// Show a new overlay. Dismisses any existing overlay first.
    /// `measureRenderer` is a renderer the overlay can use to measure text
    /// at construction time (e.g. for hex-input width). It does not need
    /// to match the renderer used at draw time.
    void open(std::unique_ptr<Overlay> overlay, OverlayContext context,
              render::Renderer* measureRenderer);

    /// Dismiss the active overlay (fires onDismiss).
    void close();

    bool hasActive() const { return active_ != nullptr; }
    Overlay* active() const { return active_.get(); }

    // --- Input dispatch. Each returns true if consumed.
    // Clicking outside a dismiss-on-click-outside overlay is consumed
    // (to eat the click that closed it) unless the overlay is still open
    // afterwards.

    bool handleMouseDown(float x, float y, int button);
    bool handleMouseMove(float x, float y);
    bool handleMouseUp(float x, float y, int button);
    bool handleWheel(float x, float y, float dx, float dy);
    bool handleKeyDown(int keycode, int mod);
    bool handleTextInput(const std::string& text);

    /// Draw the active overlay if its context matches.
    void drawIfContext(OverlayContext ctx, render::Renderer* r);

private:
    void checkDismissRequest();
    std::unique_ptr<Overlay> active_;
};

} // namespace bro::engine

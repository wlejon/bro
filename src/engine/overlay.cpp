#include "engine/overlay.h"

#include <SDL3/SDL_keycode.h>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Overlay defaults
// ---------------------------------------------------------------------------

bool Overlay::pointInBounds(float x, float y) const {
    float bx, by, bw, bh;
    getBounds(bx, by, bw, bh);
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

bool Overlay::onMouseDown(float /*x*/, float /*y*/, int /*button*/) { return false; }
bool Overlay::onMouseMove(float /*x*/, float /*y*/)                 { return false; }
bool Overlay::onMouseUp(float /*x*/, float /*y*/, int /*button*/)   { return false; }
bool Overlay::onWheel(float /*x*/, float /*y*/, float /*dx*/, float /*dy*/) { return false; }
bool Overlay::onKeyDown(int /*keycode*/, int /*mod*/)               { return false; }
bool Overlay::onTextInput(const std::string& /*text*/)              { return false; }

// ---------------------------------------------------------------------------
// OverlayManager
// ---------------------------------------------------------------------------

void OverlayManager::open(std::unique_ptr<Overlay> overlay,
                          OverlayContext context,
                          render::Renderer* measureRenderer) {
    if (active_) {
        active_->onDismiss();
        active_.reset();
    }
    if (!overlay) return;
    overlay->setContext(context);
    overlay->setMeasureRenderer(measureRenderer);
    active_ = std::move(overlay);
}

void OverlayManager::close() {
    if (!active_) return;
    active_->onDismiss();
    active_.reset();
}

void OverlayManager::checkDismissRequest() {
    if (active_ && active_->dismissRequested()) {
        close();
    }
}

bool OverlayManager::handleMouseDown(float x, float y, int button) {
    if (!active_) return false;

    if (active_->pointInBounds(x, y)) {
        active_->onMouseDown(x, y, button);
        checkDismissRequest();
        return true;
    }

    // Click outside: consume it too, so the element under the cursor doesn't
    // receive a spurious click from the dismissal gesture.
    if (active_->dismissesOnClickOutside()) {
        close();
        return true;
    }
    return false;
}

bool OverlayManager::handleMouseMove(float x, float y) {
    if (!active_) return false;
    // Forward mousemove unconditionally so drags that started inside the
    // overlay can continue outside its bounds. Returns true only if the
    // overlay's own state changed, so callers can avoid unnecessary repaints.
    bool consumed = active_->onMouseMove(x, y);
    checkDismissRequest();
    return consumed;
}

bool OverlayManager::handleMouseUp(float x, float y, int button) {
    if (!active_) return false;
    active_->onMouseUp(x, y, button);
    bool inside = active_->pointInBounds(x, y);
    checkDismissRequest();
    return inside;
}

bool OverlayManager::handleWheel(float x, float y, float dx, float dy) {
    if (!active_) return false;
    if (!active_->pointInBounds(x, y)) return false;
    active_->onWheel(x, y, dx, dy);
    checkDismissRequest();
    return true;
}

bool OverlayManager::handleKeyDown(int keycode, int mod) {
    if (!active_) return false;
    if (keycode == SDLK_ESCAPE && active_->dismissesOnEscape()) {
        close();
        return true;
    }
    bool handled = active_->onKeyDown(keycode, mod);
    checkDismissRequest();
    return handled;
}

bool OverlayManager::handleTextInput(const std::string& text) {
    if (!active_) return false;
    bool handled = active_->onTextInput(text);
    checkDismissRequest();
    return handled;
}

void OverlayManager::drawIfContext(OverlayContext ctx, render::Renderer* r) {
    if (!active_ || !r) return;
    if (active_->context() != ctx) return;
    r->save();
    r->resetClip();
    active_->draw(r);
    r->restore();
}

} // namespace bro::engine

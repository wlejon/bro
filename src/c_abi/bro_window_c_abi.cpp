// =============================================================================
// bro_window_c_abi.cpp — C++ forwarding implementations for bro.window
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_window_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static const char* s_fallback_window_state = "normal";
static bool s_fallback_borderless = false;
static bool s_fallback_always_on_top = false;
static int32_t s_fallback_pos_x = 0;
static int32_t s_fallback_pos_y = 0;
static int32_t s_fallback_min_w = 0;
static int32_t s_fallback_min_h = 0;
static int32_t s_fallback_max_w = 0;
static int32_t s_fallback_max_h = 0;

const char* bro_window_get_state(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getState) return b->getState();
    return s_fallback_window_state;
}

bool bro_window_get_borderless(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getBorderless) return b->getBorderless();
    return s_fallback_borderless;
}

void bro_window_set_borderless(bool val) {
    const auto* b = bro_get_window_bridge();
    if (b && b->setBorderless) {
        b->setBorderless(val);
    } else {
        s_fallback_borderless = val;
    }
}

bool bro_window_get_alwaysOnTop(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getAlwaysOnTop) return b->getAlwaysOnTop();
    return s_fallback_always_on_top;
}

void bro_window_set_alwaysOnTop(bool val) {
    const auto* b = bro_get_window_bridge();
    if (b && b->setAlwaysOnTop) {
        b->setAlwaysOnTop(val);
    } else {
        s_fallback_always_on_top = val;
    }
}

void bro_window_minimize(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->minimize) b->minimize();
}

void bro_window_maximize(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->maximize) b->maximize();
}

void bro_window_restore(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->restore) b->restore();
}

void* bro_window_getPosition(void) {
    return nullptr;
}

int32_t bro_window_getPositionX(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getPositionX) return b->getPositionX();
    return s_fallback_pos_x;
}

int32_t bro_window_getPositionY(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getPositionY) return b->getPositionY();
    return s_fallback_pos_y;
}

void bro_window_setPosition(int32_t x, int32_t y) {
    const auto* b = bro_get_window_bridge();
    if (b && b->setPosition) {
        b->setPosition(x, y);
    } else {
        s_fallback_pos_x = x;
        s_fallback_pos_y = y;
    }
}

void* bro_window_getMinSize(void) {
    return nullptr;
}

int32_t bro_window_getMinWidth(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getMinWidth) return b->getMinWidth();
    return s_fallback_min_w;
}

int32_t bro_window_getMinHeight(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getMinHeight) return b->getMinHeight();
    return s_fallback_min_h;
}

void bro_window_setMinSize(int32_t width, int32_t height) {
    const auto* b = bro_get_window_bridge();
    if (b && b->setMinSize) {
        b->setMinSize(width, height);
    } else {
        s_fallback_min_w = width;
        s_fallback_min_h = height;
    }
}

void* bro_window_getMaxSize(void) {
    return nullptr;
}

int32_t bro_window_getMaxWidth(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getMaxWidth) return b->getMaxWidth();
    return s_fallback_max_w;
}

int32_t bro_window_getMaxHeight(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getMaxHeight) return b->getMaxHeight();
    return s_fallback_max_h;
}

void bro_window_setMaxSize(int32_t width, int32_t height) {
    const auto* b = bro_get_window_bridge();
    if (b && b->setMaxSize) {
        b->setMaxSize(width, height);
    } else {
        s_fallback_max_w = width;
        s_fallback_max_h = height;
    }
}

void* bro_window_getDisplays(void) {
    return nullptr;
}

int32_t bro_window_getDisplayCount(void) {
    const auto* b = bro_get_window_bridge();
    if (b && b->getDisplayCount) return b->getDisplayCount();
    return 1;
}

bool bro_window_moveToDisplay(uint32_t id) {
    const auto* b = bro_get_window_bridge();
    if (b && b->moveToDisplay) return b->moveToDisplay(id);
    return true;
}

} // extern "C"

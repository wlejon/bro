// =============================================================================
// bro_gizmo_c_abi.cpp — C++ forwarding implementations for bro.gizmo
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_gizmo_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>

extern "C" {

static bool s_fallback_gizmo_visible = false;
static bool s_fallback_gizmo_dragging = false;
static std::string s_fallback_gizmo_hovered;
static std::string s_fallback_gizmo_mode = "translate";
static std::string s_fallback_gizmo_space = "world";

bool bro_gizmo_get_visible(void) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->getVisible) return b->getVisible();
    return s_fallback_gizmo_visible;
}

bool bro_gizmo_get_dragging(void) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->getDragging) return b->getDragging();
    return s_fallback_gizmo_dragging;
}

const char* bro_gizmo_get_hovered(void) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->getHovered) return b->getHovered();
    return s_fallback_gizmo_hovered.empty() ? nullptr : s_fallback_gizmo_hovered.c_str();
}

void bro_gizmo_show(void) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->show) {
        b->show();
    } else {
        s_fallback_gizmo_visible = true;
    }
}

void bro_gizmo_hide(void) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->hide) {
        b->hide();
    } else {
        s_fallback_gizmo_visible = false;
    }
}

void bro_gizmo_setMode(const char* mode) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->setMode) {
        b->setMode(mode);
    } else if (mode) {
        s_fallback_gizmo_mode = mode;
    }
}

void bro_gizmo_setSpace(const char* space) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->setSpace) {
        b->setSpace(space);
    } else if (space) {
        s_fallback_gizmo_space = space;
    }
}

void bro_gizmo_setPosition(double x, double y, double z) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->setPosition) b->setPosition(x, y, z);
}

void bro_gizmo_setOrientation(double x, double y, double z, double w) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->setOrientation) b->setOrientation(x, y, z, w);
}

void bro_gizmo_configure(void* config) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->configure) b->configure(config);
}

void bro_gizmo_attach(void* handlers) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->attach) {
        b->attach(handlers);
    } else {
        s_fallback_gizmo_visible = true;
    }
}

void bro_gizmo_detach(void) {
    const auto* b = bro_get_gizmo_bridge();
    if (b && b->detach) {
        b->detach();
    } else {
        s_fallback_gizmo_visible = false;
    }
}

} // extern "C"
// =============================================================================
// bro_menu_c_abi.cpp — C++ forwarding implementations for bro.menu
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_menu_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

static bool s_fallback_menu_visible = false;

void bro_menu_show(void) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->show) {
        b->show();
    } else {
        s_fallback_menu_visible = true;
    }
}

void bro_menu_hide(void) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->hide) {
        b->hide();
    } else {
        s_fallback_menu_visible = false;
    }
}

bool bro_menu_get_visible(void) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->getVisible) {
        return b->getVisible();
    }
    return s_fallback_menu_visible;
}

void bro_menu_set(void* items) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->set) b->set(items);
}

bool bro_menu_addItem(const char* parentId, void* item, int32_t index) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->addItem) return b->addItem(parentId, item, index);
    return true;
}

bool bro_menu_updateItem(const char* id, void* props) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->updateItem) return b->updateItem(id, props);
    return true;
}

bool bro_menu_removeItem(const char* id) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->removeItem) return b->removeItem(id);
    return true;
}

void bro_menu_on(const char* id, void* callback) {
    const auto* b = bro_get_menu_bridge();
    if (b && b->on) b->on(id, callback);
}

} // extern "C"

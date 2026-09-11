// =============================================================================
// bro_dialogs_c_abi.cpp — C++ forwarding implementations for bro.dialogs
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_dialogs_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>

extern "C" {

static std::string s_fallback_dialog_str;

void bro_dialogs_alert(const char* message) {
    const auto* b = bro_get_dialogs_bridge();
    if (b && b->alert) {
        b->alert(message ? message : "");
    }
}

bool bro_dialogs_confirm(const char* message) {
    const auto* b = bro_get_dialogs_bridge();
    if (b && b->confirm) {
        return b->confirm(message ? message : "");
    }
    return true;
}

const char* bro_dialogs_prompt(const char* message, const char* defaultText) {
    const auto* b = bro_get_dialogs_bridge();
    if (b && b->prompt) {
        return b->prompt(message ? message : "", defaultText ? defaultText : "");
    }
    s_fallback_dialog_str = defaultText ? defaultText : "";
    return s_fallback_dialog_str.c_str();
}

const char* bro_dialogs_showSaveFileDialog(const char* filter, const char* defaultName) {
    const auto* b = bro_get_dialogs_bridge();
    if (b && b->showSaveFileDialog) {
        return b->showSaveFileDialog(filter ? filter : "", defaultName ? defaultName : "");
    }
    s_fallback_dialog_str = defaultName ? defaultName : "";
    return s_fallback_dialog_str.c_str();
}

const char* bro_dialogs_showOpenFileDialog(const char* filter, bool allowMultiple) {
    const auto* b = bro_get_dialogs_bridge();
    if (b && b->showOpenFileDialog) {
        return b->showOpenFileDialog(filter ? filter : "", allowMultiple);
    }
    return "";
}

const char* bro_dialogs_showOpenFolderDialog(const char* defaultLocation, bool allowMultiple) {
    const auto* b = bro_get_dialogs_bridge();
    if (b && b->showOpenFolderDialog) {
        return b->showOpenFolderDialog(defaultLocation ? defaultLocation : "", allowMultiple);
    }
    s_fallback_dialog_str = defaultLocation ? defaultLocation : "";
    return s_fallback_dialog_str.c_str();
}

} // extern "C"

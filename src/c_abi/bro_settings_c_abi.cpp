// =============================================================================
// bro_settings_c_abi.cpp — C++ forwarding implementations for bro.settings
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_settings_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>
#include <unordered_map>

extern "C" {

static std::unordered_map<std::string, std::string> s_fallback_settings;
static std::string s_fallback_settings_str;

void bro_settings_load(void) {
    const auto* b = bro_get_settings_bridge();
    if (b && b->load) b->load();
}

void bro_settings_save(void) {
    const auto* b = bro_get_settings_bridge();
    if (b && b->save) b->save();
}

const char* bro_settings_get(const char* key) {
    if (!key) return "";
    const auto* b = bro_get_settings_bridge();
    if (b && b->get) return b->get(key);
    auto it = s_fallback_settings.find(key);
    if (it != s_fallback_settings.end()) {
        s_fallback_settings_str = it->second;
        return s_fallback_settings_str.c_str();
    }
    return "";
}

void bro_settings_set(const char* key, const char* val) {
    if (!key || !val) return;
    const auto* b = bro_get_settings_bridge();
    if (b && b->set) {
        b->set(key, val);
    } else {
        s_fallback_settings[key] = val;
    }
}

void bro_settings_reset(const char* category) {
    const auto* b = bro_get_settings_bridge();
    if (b && b->reset) {
        b->reset(category ? category : "");
    } else {
        if (!category || !*category) {
            s_fallback_settings.clear();
        } else {
            std::string prefix = std::string(category) + ".";
            for (auto it = s_fallback_settings.begin(); it != s_fallback_settings.end();) {
                if (it->first.rfind(prefix, 0) == 0 || it->first == category) {
                    it = s_fallback_settings.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

bool bro_settings_isActionPressed(const char* action) {
    if (!action) return false;
    const auto* b = bro_get_settings_bridge();
    if (b && b->isActionPressed) return b->isActionPressed(action);
    return false;
}

double bro_settings_getActionStrength(const char* action) {
    if (!action) return 0.0;
    const auto* b = bro_get_settings_bridge();
    if (b && b->getActionStrength) return b->getActionStrength(action);
    return 0.0;
}

} // extern "C"

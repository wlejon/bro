// =============================================================================
// bro_paths_c_abi.cpp — C++ forwarding implementations for bro.paths
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_paths_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>

extern "C" {

static std::string s_fallback_appDir = ".";
static std::string s_fallback_userDataDir = ".";
static std::string s_fallback_resolved;

const char* bro_paths_get_appDir(void) {
    const auto* b = bro_get_paths_bridge();
    if (b && b->getAppDir) return b->getAppDir();
    return s_fallback_appDir.c_str();
}

const char* bro_paths_get_userDataDir(void) {
    const auto* b = bro_get_paths_bridge();
    if (b && b->getUserDataDir) return b->getUserDataDir();
    return s_fallback_userDataDir.c_str();
}

const char* bro_paths_resolvePath(const char* src) {
    if (!src) return "";
    const auto* b = bro_get_paths_bridge();
    if (b && b->resolvePath) return b->resolvePath(src);
    s_fallback_resolved = src;
    return s_fallback_resolved.c_str();
}

const char* bro_paths_resolveWritePath(const char* src) {
    if (!src) return "";
    const auto* b = bro_get_paths_bridge();
    if (b && b->resolveWritePath) return b->resolveWritePath(src);
    s_fallback_resolved = src;
    return s_fallback_resolved.c_str();
}

} // extern "C"

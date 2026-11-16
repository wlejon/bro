#pragma once

#include "engine/engine.h"
#include <cstdlib>
#include <filesystem>
#include <string>

namespace bro::bronze_host {

inline std::string discoverPinsPath(const engine::Engine& engine, const std::filesystem::path& targetScriptOrDir) {
    if (const char* env = std::getenv("BRO_PINS")) {
        if (env[0] != '\0') return env;
    }
    std::error_code ec;
    std::filesystem::path candidate;
    if (std::filesystem::is_directory(targetScriptOrDir, ec)) {
        candidate = targetScriptOrDir / "app.pins";
    } else if (!targetScriptOrDir.empty()) {
        candidate = targetScriptOrDir.parent_path() / "app.pins";
    }
    if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
        return candidate.string();
    }
    if (!engine.appDir().empty()) {
        auto appCandidate = std::filesystem::path(engine.appDir()) / "app.pins";
        if (std::filesystem::exists(appCandidate, ec)) {
            return appCandidate.string();
        }
    }
    return {};
}

}  // namespace bro::bronze_host

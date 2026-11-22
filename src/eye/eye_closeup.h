#pragma once

#include "eye/eye_types.h"

#include <string>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeCloseupTargetInfo {
    std::string query;
    int matchedNodes = 0;
    double center[3] = {0.0, 0.0, 0.0};
    double extent[3] = {0.0, 0.0, 0.0};
    double minY = 0.0;
    double maxY = 0.0;
};

struct EyeCloseupOptions {
    std::vector<std::string> targets;
    std::vector<std::string> angles = {"threeQuarter", "grazing"};
    double margin = 2.35;
    int maxPanelDim = 520;
    int maxPanels = 12;
    std::string path;
};

struct EyeCloseupResult {
    bool ok = true;
    int panelCount = 0;
    std::vector<EyeCloseupTargetInfo> targets;
    EyeImage visual;
    std::string visualPath;
};

/// Render tight multi-angle close-up contact sheets around named (or auto-selected) 3D scene subjects
/// so micro-seams, flat caps, levitating parts, and underside attachment gaps are clearly visible.
EyeCloseupResult captureCloseup(scene::SceneGraph& graph, const EyeCloseupOptions& opts = {});

}  // namespace bro::eye

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
    std::string mode = "targets";  // "targets" | "inventory"
    std::vector<std::string> targets;
    std::vector<std::string> angles = {"threeQuarter", "grazing"};
    double margin = 2.35;
    int maxPanelDim = 360;
    int maxPanels = 12;
    int sheetWidth = 768;
    int sheetHeight = 768;
    bool isolate = false;
    bool showScale = true;
    bool showHeading = false;
    std::string path;
};

struct EyeCloseupResult {
    bool ok = true;
    int width = 0;
    int height = 0;
    int panelCount = 0;
    std::vector<EyeCloseupTargetInfo> targets;
    EyeImage visual;
    std::string visualPath;
};

/// Render tight multi-angle close-up contact sheets around named or auto-discovered 3D scene subjects
/// (optionally isolating each subject and overlaying metric scale bars and local heading axes)
/// inside an exact native-tile image so zero post-scaling is needed on vision ingest.
EyeCloseupResult captureCloseup(scene::SceneGraph& graph, const EyeCloseupOptions& opts = {});

}  // namespace bro::eye

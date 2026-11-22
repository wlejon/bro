#pragma once

#include "eye/eye_types.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeLookdevOptions {
    std::string target;
    std::string angle = "threeQuarter";
    bool isolate = false;
    double margin = 2.25;
    int sheetWidth = 768;
    int sheetHeight = 768;
    std::string path;
};

struct EyeMaterialAuditItem {
    std::string name;
    bool hasAlbedoMap = false;
    bool hasNormalMap = false;
    bool hasMRMap = false;
    bool castsShadow = true;
    bool translucent = false;
};

struct EyeLookdevResult {
    bool ok = true;
    int width = 0;
    int height = 0;
    int matchedNodes = 0;
    double roughnessMean = 0.0;
    double roughnessStd = 0.0;
    double glossyFrac = 0.0;
    double valueMean = 0.0;
    double valueStd = 0.0;
    double valueContrast = 0.0;
    double albedoSaturationMean = 0.0;
    double silhouetteEdgeDensity = 0.0;
    double clippedWhiteFrac = 0.0;
    int bloomHotspotCount = 0;
    int checkedMeshes = 0;
    int untexturedCount = 0;
    int noNormalMapCount = 0;
    int noShadowSolidCount = 0;
    std::vector<EyeMaterialAuditItem> materialWarnings;
    EyeImage visual;
    std::string visualPath;
};

struct EyeSweepOptions {
    int sheetWidth = 768;
    int sheetHeight = 0;  // 0 = auto (432 for <=3 states, 768 for 4+)
    std::string path;
};

struct EyeSweepStateResult {
    std::string label;
    double valueMean = 0.0;
    double valueContrast = 0.0;
    double clippedWhiteFrac = 0.0;
    int hotspotCount = 0;
};

struct EyeSweepResult {
    bool ok = true;
    int width = 0;
    int height = 0;
    int stateCount = 0;
    std::vector<EyeSweepStateResult> states;
    EyeImage visual;
    std::string visualPath;
};

/// Render an exact 2x2 PBR lookdev breakdown (Beauty, Albedo, Roughness, Squint/Value) for a
/// specific target or the live view, with quantitative surface statistics and optional native PNG.
EyeLookdevResult captureLookdev(scene::SceneGraph& graph, const EyeLookdevOptions& opts = {});

/// Render a compact multi-state/mood comparison sheet (`applyFn()` per state) with HDR spike
/// and luminance/contrast checks in a single exact native-tile image.
EyeSweepResult captureStateSweep(
    scene::SceneGraph& graph, const EyeSweepOptions& opts,
    const std::vector<std::pair<std::string, std::function<void()>>>& states);

}  // namespace bro::eye

#pragma once

#include "eye/eye_types.h"

#include <string>

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
    EyeImage visual;
    std::string visualPath;
};

/// Render an exact 2x2 PBR lookdev breakdown (Beauty, Albedo, Roughness, Squint/Value) for a
/// specific target or the live view, with quantitative surface statistics and optional native PNG.
EyeLookdevResult captureLookdev(scene::SceneGraph& graph, const EyeLookdevOptions& opts = {});

}  // namespace bro::eye

#pragma once

#include "eye/eye_types.h"

#include <string>
#include <vector>

namespace bro::eye {

struct EyeMotionOptions {
    double threshold = 3.0;         // Oklab deltaE*100 pixel change threshold
    std::vector<std::string> labels;
    std::vector<double> timestampsMs;
    int maxPanelDim = 480;
    std::string layout = "grid";    // "side" | "grid"
    std::string visualStyle = "board";  // "board" | "chrono" | "kymograph" | "sheet"
    double slitX = -1.0;            // normalized 0..1 (< 0 picks peak motion col)
    double slitY = -1.0;            // normalized 0..1 (< 0 picks peak motion row)
    bool buildVisual = false;
    std::string path;               // optional output PNG path
};

struct EyeMotionStep {
    int fromIndex = 0;
    int toIndex = 0;
    double deltaEMean = 0.0;
    double deltaEP95 = 0.0;
    double changedRatio = 0.0;
    double ssim = 1.0;
    double lumaDiff = 0.0;
    double dtMs = 0.0;
    double centroidX = 0.5;
    double centroidY = 0.5;
    bool staticStep = false;
    bool stutter = false;
    bool pop = false;
    bool hiccup = false;
};

struct EyeMotionResult {
    int width = 0;
    int height = 0;
    int frameCount = 0;
    int staticPairCount = 0;
    int stutterCount = 0;
    int popCount = 0;
    int hiccupCount = 0;
    double meanDeltaE = 0.0;
    double deltaEStd = 0.0;
    double smoothness = 0.0;        // 0..1 (higher = even cadence across steps)
    double lumaMin = 0.0;
    double lumaMax = 0.0;
    double lumaRange = 0.0;
    double flickerScore = 0.0;      // 0..1 (high-frequency luminance reversal)
    double motionCoverage = 0.0;    // fraction of pixels touched by motion across sequence
    double dtMedianMs = 0.0;
    double dtP95Ms = 0.0;
    double dtMaxMs = 0.0;
    double fpsMean = 0.0;
    double autoSlitX = 0.5;
    double autoSlitY = 0.5;
    EyeRect motionBounds{};
    std::vector<EyeMotionStep> steps;
    EyeImage visual;
    std::string visualPath;
};

/// Analyze temporal smoothness, stutter, popping, flicker, and spatial coverage
/// across an ordered sequence of RGBA8 frames.
EyeMotionResult analyzeMotion(const std::vector<EyeImageView>& frames, const EyeMotionOptions& opts = {});

}  // namespace bro::eye

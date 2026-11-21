#pragma once

#include "eye/eye_taste_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::eye {

struct EyeImageView {
    const uint8_t* rgba = nullptr;
    int width = 0;
    int height = 0;
    int strideBytes = 0;

    bool valid() const { return rgba != nullptr && width > 0 && height > 0; }
    int effectiveStride() const { return strideBytes > 0 ? strideBytes : width * 4; }
};

struct EyeImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return rgba.empty() || width <= 0 || height <= 0; }
    EyeImageView view() const { return {rgba.data(), width, height, width * 4}; }
};

struct EyeRect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

struct EyeDominantColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    std::string hex;
    double L = 0.0;
    double C = 0.0;
    double h = 0.0;
    double share = 0.0;
};

struct EyeTiling {
    double score = 0.0;           // 0..1 (higher = stronger periodic repetition)
    double peakRatio = 0.0;       // max radial-normalized off-DC FFT peak
    double periodX = 0.0;         // dominant period in pixels (0 if none)
    double periodY = 0.0;
    double horizontalBias = 0.0;  // 0..1
    double verticalBias = 0.0;    // 0..1
};

struct EyeSeam {
    double score = 0.0;           // 0..1 (0 = seamless wrap, >0.35 = visible border seam)
    double hSeam = 0.0;           // top<->bottom wrap seam score (0..1)
    double vSeam = 0.0;           // left<->right wrap seam score (0..1)
    double edgeDeltaE = 0.0;      // mean Oklab deltaE across wrapped borders
    double interiorDeltaE = 0.0;  // baseline adjacent-pixel Oklab deltaE inside image
};

struct EyeDetailBands {
    double low = 0.0;             // macro / large-scale structure energy share (0..1)
    double mid = 0.0;             // medium-scale texture energy share (0..1)
    double high = 0.0;            // crisp micro-detail energy share (0..1)
};

struct EyeBalance {
    double centerOfMassX = 0.5;   // normalized 0..1
    double centerOfMassY = 0.5;
    double leftRightDiff = 0.0;   // -1..1 (+ = heavier on right)
    double topBottomDiff = 0.0;   // -1..1 (+ = heavier on bottom)
    double thirdsEnergy = 0.0;    // fraction of visual mass near rule-of-thirds points
};

struct EyeGridCell {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    double lumaMean = 0.0;
    double chromaMean = 0.0;
    double edgeDensity = 0.0;
    double crushedBlackRatio = 0.0;
    double blownWhiteRatio = 0.0;
};

struct EyeMeasureOptions {
    int gridRows = 3;
    int gridCols = 3;
    int maxColors = 5;
    bool detectTiling = true;
};

struct EyeMeasurement {
    int width = 0;
    int height = 0;
    int64_t pixels = 0;
    double aspect = 1.0;

    // Luminance / exposure (Oklab L in 0..1)
    double lumaMean = 0.0;
    double lumaStd = 0.0;
    double lumaMin = 0.0;
    double lumaMax = 0.0;
    double lumaP05 = 0.0;
    double lumaP50 = 0.0;
    double lumaP95 = 0.0;
    double dynamicRange = 0.0;       // lumaP95 - lumaP05
    double exposureBias = 0.0;       // lumaMean - 0.5
    double crushedBlackRatio = 0.0;
    double blownWhiteRatio = 0.0;
    std::array<double, 10> zones{};  // Ansel Adams luminance histogram (0..1 sum)
    double bandingScore = 0.0;       // 0..1

    // Color (Oklab C, h)
    double chromaMean = 0.0;
    double chromaStd = 0.0;
    double chromaP95 = 0.0;
    double colorfulRatio = 0.0;      // fraction with C > 0.04
    double hueEntropy = 0.0;         // 0..1 normalized 12-bin hue entropy
    double warmCoolBalance = 0.0;    // -1 (cool) .. +1 (warm)
    std::vector<EyeDominantColor> dominantColors;

    // Structure / detail
    double sharpness = 0.0;          // normalized Laplacian sharpness
    double laplacianVariance = 0.0;
    double tenengrad = 0.0;
    double edgeDensity = 0.0;        // fraction of pixels with significant gradient
    double clutterScore = 0.0;       // 0..1 high-frequency / busyness index
    double whitespaceRatio = 0.0;    // fraction of calm low-gradient surface

    EyeTiling tiling;
    EyeSeam seam;
    EyeDetailBands detailBands;
    EyeBalance balance;
    std::vector<EyeGridCell> grid;
};

struct EyeCompareOptions {
    int gridRows = 3;
    int gridCols = 3;
    int changeThreshold = 8;
    double ssimWeight = 0.35;
    double deltaEWeight = 0.35;
    double exposureWeight = 0.15;
    double colorWeight = 0.15;
};

struct EyeCompareCell {
    int x = 0, y = 0, w = 0, h = 0;
    double ssim = 1.0;
    double deltaEMean = 0.0;
    double lumaDiff = 0.0;
    double changedRatio = 0.0;
};

struct EyeComparison {
    double score = 0.0;           // 0 = identical, ~0.1 = close, >0.4 = very different
    bool sameSize = true;

    double ssim = 1.0;
    double ssimDist = 0.0;        // 1 - clamp(ssim, 0, 1)
    double deltaEMean = 0.0;      // Oklab deltaE * 100
    double deltaEP95 = 0.0;
    double deltaEMax = 0.0;
    double changedRatio = 0.0;
    double psnr = 0.0;
    double rms = 0.0;

    double exposureDist = 0.0;    // 1D EMD over zones (0..1)
    double colorDist = 0.0;       // palette/chroma/hue distance (0..1+)
    double detailDist = 0.0;      // edge/sharpness/clutter distance (0..1+)

    // Directional breakdown
    double lumaDiff = 0.0;        // b.lumaMean - a.lumaMean
    double contrastRatio = 1.0;   // b.lumaStd / a.lumaStd
    double chromaRatio = 1.0;     // b.chromaMean / a.chromaMean
    double sharpnessRatio = 1.0;  // b.sharpness / a.sharpness
    double tilingDiff = 0.0;      // b.tiling.score - a.tiling.score

    EyePerceptualDiff perceptual;
    std::vector<EyeCompareCell> cells;
};

struct EyeIssue {
    std::string kind;
    std::string severity;         // "error" | "warning"
    std::string selector;
    std::string message;
    EyeRect rect;

    bool hasContrast = false;
    double wcag = 0.0;
    double apca = 0.0;
    std::string fgHex;
    std::string bgHex;
    double fontSize = 0.0;
    std::string text;

    bool hasOverflow = false;
    double overflowX = 0.0;
    double overflowY = 0.0;

    bool hasOverlap = false;
    double overlapRatio = 0.0;
    std::string otherSelector;

    bool hasSpatial = false;
    std::string nodeName;
    std::string otherNodeName;
    double gap = 0.0;
    double value = 0.0;
};

struct EyeAuditOptions {
    std::string selector;         // optional root selector (empty = document root)
    double minContrast = 4.5;     // WCAG AA normal text (large text automatically uses minContrast * 3/4.5)
    double minApca = 0.0;         // optional minimum |Lc| (e.g. 45)
    double minHitTarget = 24.0;   // minimum interactive box size in CSS px
    double minOverlapPx = 16.0;   // minimum intersection area in CSS px^2
    bool checkOverlap = true;
    bool checkClip = true;
    bool checkOffscreen = true;
    bool checkHitTargets = true;
    bool checkFrame = true;
    int maxIssues = 64;
};

struct EyeAuditResult {
    bool ok = true;
    double score = 100.0;
    int errorCount = 0;
    int warningCount = 0;
    int checkedTextNodes = 0;
    int checkedElements = 0;
    std::vector<EyeIssue> issues;
    EyeMeasurement frame;
};

struct EyeSceneOptions {
    std::string focus;            // optional node name or substring for silhouette/framing focus
    bool render = true;           // call renderForRead() before reading snapshot
    bool checkStability = false;  // sub-pixel camera jitter check (re-renders + restores camera)
    bool checkFraming = true;     // check focus clipping, under-framing, and form modeling
    double jitterPx = 0.35;
    double minSilhouetteContrast = 12.0;  // minimum mean boundary deltaE
    double floatMin = 0.02;
    double floatMax = 5.0;
    double penetrationEps = 0.01;
    double uvStretchThreshold = 6.0;
    int maxIssues = 64;
};

struct EyeSceneNodeInfo {
    uint32_t id = 0;
    std::string name;
    bool visible = true;
    double screenCoverage = 0.0;
    double visibleFraction = 0.0;
    EyeRect bounds2d;
    double minDepth = 0.0;        // meters (camera-space distance)
    double maxDepth = 0.0;
    double silhouetteContrast = 0.0;
    double lumaMean = 0.0;        // Oklab L mean inside visible mesh mask
    double lumaStd = 0.0;         // Oklab L std dev inside mask
    double formRange = 0.0;       // P95 - P05 Oklab L modeling range inside mask
    bool frameClipped = false;    // true if visible bounds touch viewport edges
};

struct EyeSceneStability {
    bool tested = false;
    double jitterPx = 0.0;
    double meanDeltaE = 0.0;
    double maxDeltaE = 0.0;
    double changedRatio = 0.0;
    double depthFlipRatio = 0.0;
    double shimmerScore = 0.0;
};

struct EyeSceneResult {
    bool ok = true;
    double score = 100.0;
    int errorCount = 0;
    int warningCount = 0;
    int width = 0;
    int height = 0;
    double skyRatio = 0.0;
    double nearDist = 0.0;
    double medianDist = 0.0;
    double farDist = 0.0;
    double exposureEv = 0.0;
    double meteredEv = 0.0;
    int meshDrawn = 0;
    int meshCulled = 0;
    std::vector<EyeSceneNodeInfo> nodes;
    std::vector<EyeIssue> issues;
    EyeSceneStability stability;
    EyeMeasurement frame;
};

struct EyeVisualPanel {
    std::string label;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct EyeVisualizeOptions {
    std::string mode;             // "audit" | "diff" | "exposure" | "tiling" | "depth" | "normals" | "sheet"
    std::string layout = "side";  // "side" | "stack" | "grid"
    std::vector<std::string> labels;
    int fontScale = 2;
    int maxPanelDim = 640;        // cap panel dimension for readable, compact sheets
    std::string path;
    const EyeAuditResult* audit = nullptr;
    const EyeSceneResult* sceneAudit = nullptr;
    const EyeComposeResult* compose = nullptr;
    const EyeFormResult* form = nullptr;
    int notanTones = 3;
    double zebraFreq = 12.0;
    double issueCoordScale = 1.0;
    const float* depth = nullptr;
    int depthWidth = 0;
    int depthHeight = 0;
    std::array<float, 16> invProj{};
    std::array<float, 16> relView{};
    bool hasCamera = false;
};

struct EyeVisualResult {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::vector<EyeVisualPanel> panels;
    std::string path;
};

}  // namespace bro::eye

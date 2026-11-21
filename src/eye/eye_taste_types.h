#pragma once

#include <array>
#include <string>
#include <vector>

namespace bro::eye {

struct EyeRect;

struct EyeFocalPoint {
    double x = 0.0;       // normalized 0..1
    double y = 0.0;       // normalized 0..1
    double radius = 0.0;  // normalized influence radius
    double strength = 0.0;
    int rank = 0;
};

struct EyeSaliency {
    double centerOfMassX = 0.5;
    double centerOfMassY = 0.5;
    double balanceX = 0.0;        // centerOfMassX - 0.5
    double balanceY = 0.0;        // centerOfMassY - 0.5
    double focalDominance = 1.0;  // top1 / mean(top2..N)
    double edgeCrowding = 0.0;    // fraction of saliency in outer 5% frame margin
    std::vector<EyeFocalPoint> anchors;
};

struct EyeNotan {
    double darkShare = 0.0;
    double midShare = 0.0;
    double lightShare = 0.0;
    double largestMassShare = 0.0;
    double fragmentation = 0.0;   // 0..1 (higher = many tiny disconnected blobs)
    int massCount = 0;
    std::array<double, 5> tone5Shares{};
};

struct EyeRhythm {
    double restRatio = 0.0;           // calm low-frequency cells
    double detailRatio = 0.0;         // high-detail active cells
    double restDetailContrast = 0.0;  // P90 - P10 of cell edge activity
    int gridRows = 6;
    int gridCols = 8;
};

struct EyePaletteHarmony {
    double minTierDeltaE = 0.0;     // smallest Oklab dE between adjacent dominant colors
    double medianTierDeltaE = 0.0;
    double warmShadowShift = 0.0;   // warm(highlights) - warm(shadows) in Oklab (a+b)
    double chromaRestraint = 1.0;   // 1 - clamp(chromaP95 / 0.25, 0, 1)
    int tierCount = 0;
};

struct EyeRail {
    std::string axis;  // "x" | "y"
    double pos = 0.0;
    int count = 0;
};

struct EyeRailDrift {
    std::string axis;
    std::string selector;
    double pos = 0.0;
    double railPos = 0.0;
    double driftPx = 0.0;
    double rectX = 0.0, rectY = 0.0, rectW = 0.0, rectH = 0.0;
};

struct EyeGapOutlier {
    std::string parentSelector;
    std::string axis;
    double gapPx = 0.0;
    double expectedPx = 0.0;
    double rectX = 0.0, rectY = 0.0, rectW = 0.0, rectH = 0.0;
};

struct EyeWrapIssue {
    std::string selector;
    std::string text;
    double width = 0.0;
    double height = 0.0;
    int estimatedLines = 0;
    double rectX = 0.0, rectY = 0.0, rectW = 0.0, rectH = 0.0;
};

struct EyeTangentIssue {
    std::string selectorA;
    std::string selectorB;
    double gapPx = 0.0;
    double rectX = 0.0, rectY = 0.0, rectW = 0.0, rectH = 0.0;
};

struct EyeLayoutAnalysis {
    bool analyzed = false;
    int elementCount = 0;
    int xRailCount = 0;
    int yRailCount = 0;
    double railEconomy = 1.0;        // 1 - min(1, uniqueRails / max(1, elementCount * 2))
    double spacingRegularity = 1.0;  // fraction of sibling gaps aligned to modal step
    double gestaltRatio = 1.0;       // median inner-group gap / median outer-sibling gap
    std::vector<EyeRail> rails;
    std::vector<EyeRailDrift> drifts;
    std::vector<EyeGapOutlier> gapOutliers;
    std::vector<EyeWrapIssue> wrapIssues;
    std::vector<EyeTangentIssue> tangents;
};

struct EyeComposeOptions {
    int maxAnchors = 5;
    int notanTones = 3;
    int rhythmRows = 6;
    int rhythmCols = 8;
    bool includeLayout = false;
    std::string selector;
    double railTolerancePx = 1.5;
    double driftMaxPx = 6.0;
};

struct EyeComposeResult {
    int width = 0;
    int height = 0;
    double score = 100.0;
    EyeSaliency saliency;
    EyeNotan notan;
    EyeRhythm rhythm;
    EyePaletteHarmony palette;
    EyeLayoutAnalysis layout;
};

struct EyeSurfaceDefect {
    std::string kind;  // "g1-crease" | "curvature-pinch" | "flat-spot" | "non-manifold"
    double x = 0.0, y = 0.0, z = 0.0;
    double value = 0.0;
    std::string message;
};

struct EyeFormSurface {
    int vertexCount = 0;
    int triangleCount = 0;
    int nonManifoldEdges = 0;
    int creaseEdgeCount = 0;
    int pinchVertexCount = 0;
    double surfaceArea = 0.0;
    double meanCurvatureAbs = 0.0;
    double gaussianCurvatureAbs = 0.0;
    double ellipticRatio = 0.0;
    double hyperbolicRatio = 0.0;
    double parabolicRatio = 0.0;
    double flatRatio = 0.0;
    double creaseRatio = 0.0;
    double highFreqRoughness = 0.0;
    double smoothnessScore = 1.0;
};

struct EyeFormMass {
    double volume = 0.0;
    std::array<double, 3> centerOfMass{0.0, 0.0, 0.0};
    std::array<double, 3> boundsMin{0.0, 0.0, 0.0};
    std::array<double, 3> boundsMax{0.0, 0.0, 0.0};
    double heightWidthRatio = 1.0;
    double depthWidthRatio = 1.0;
    double plumbOffsetX = 0.0;
    double plumbOffsetZ = 0.0;
    double stabilityMargin = 0.0;
    double symmetryScore = 1.0;
    bool grounded = true;
};

struct EyeSliceProfile {
    double yFrac = 0.0;
    double width = 0.0;
    double depth = 0.0;
    double area = 0.0;
    double centerX = 0.0;
    double centerZ = 0.0;
};

struct EyeSilhouetteAnomaly {
    std::string kind;  // "wiggle" | "sharp-corner" | "flat-span" | "pinch"
    double u = 0.0;
    double v = 0.0;
    double value = 0.0;
};

struct EyeSilhouetteHole {
    double areaRatio = 0.0;
    double cx = 0.5;
    double cy = 0.5;
    double aspect = 1.0;
};

struct EyeSilhouetteView {
    double azimuthDeg = 0.0;
    double elevationDeg = 0.0;
    double coverage = 0.0;
    double solidity = 1.0;
    double perimeterRatio = 0.0;
    double flatRatio = 0.0;
    double smoothness = 1.0;
    int inflectionCount = 0;
    int wiggleCount = 0;
    int sharpCornerCount = 0;
    int holeCount = 0;
    int pinchCount = 0;
    std::vector<EyeSilhouetteHole> holes;
    std::vector<EyeSilhouetteAnomaly> anomalies;
};

struct EyeFormLighting {
    bool analyzed = false;
    double highlightShare = 0.0;
    double halftoneShare = 0.0;
    double coreShadowShare = 0.0;
    double reflectedLightShare = 0.0;
    double warmCoolLightShift = 0.0;
    double valueTurnScore = 0.0;
};

struct EyeFormOptions {
    std::string focus;
    int azimuthCount = 8;
    double elevationDeg = 8.0;
    double creaseAngleDeg = 35.0;
    int sliceCount = 8;
    int silhouetteRes = 192;
    bool render = true;
};

struct EyeFormResult {
    bool ok = true;
    double score = 100.0;
    std::string subjectName;
    EyeFormSurface surface;
    EyeFormMass mass;
    std::vector<EyeSliceProfile> slices;
    std::vector<EyeSilhouetteView> silhouettes;
    EyeFormLighting lighting;
    std::vector<EyeSurfaceDefect> defects;
};

struct EyePerceptualDiff {
    double focalShiftX = 0.0;
    double focalShiftY = 0.0;
    double focalShiftDist = 0.0;
    double focalDominanceDiff = 0.0;
    double notanMassDiff = 0.0;
    double restDetailDiff = 0.0;
    double balanceDiff = 0.0;
};

}  // namespace bro::eye

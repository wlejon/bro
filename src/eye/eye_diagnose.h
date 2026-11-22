#pragma once

#include "eye/eye_types.h"

#include <string>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeDiagnoseOptions {
    int minPixels = 16;
    bool alwaysRender = false;
    int sheetWidth = 768;
    int sheetHeight = 0;  // 0 = auto (384 for 1-2 crops, 768 for 3-4)
    std::string path;
};

struct EyeDiagnoseAnomaly {
    std::string kind;      // "bright_outlier" | "tint_mismatch" | "flat_plastic" | "axis_grid" | "no_shadow_caster" | "bloom_hotspot"
    std::string severity;  // "error" | "warning"
    std::string nodeName;
    uint32_t nodeId = 0;
    double value = 0.0;
    int bbox[4] = {0, 0, 0, 0};  // minX, minY, maxX, maxY
    std::string message;
};

struct EyeDiagnoseMeshStats {
    std::string name;
    uint32_t nodeId = 0;
    int screenPx = 0;
    double screenFrac = 0.0;
    double p10Lum = 0.0;
    double p50Lum = 0.0;
    double p95Lum = 0.0;
    double p99Lum = 0.0;
    double outlierBrightFrac = 0.0;
    double roughnessMean = 0.0;
    double roughnessStd = 0.0;
    double albedoLumMean = 0.0;
    double hueShiftDeg = 0.0;
    double chromaDelta = 0.0;
    double detailEntropy = 0.0;
    double normalStd = 0.0;
    double axisGridScore = 0.0;
    bool hasAlbedoMap = false;
    bool hasNormalMap = false;
    bool hasMRMap = false;
    bool castsShadow = true;
    bool translucent = false;
    double striationScore = 0.0;
};

struct EyeDiagnoseResult {
    bool ok = true;
    int width = 0;
    int height = 0;
    int visibleMeshes = 0;
    int anomalyCount = 0;
    int bloomHotspotCount = 0;
    std::vector<EyeDiagnoseAnomaly> anomalies;
    std::vector<EyeDiagnoseMeshStats> meshes;
    EyeImage visual;
    std::string visualPath;
};

/// Zero-image peripheral anomaly detector over a live SceneGraph using GPU object-ID attribution.
/// Computes per-mesh interior luminance quantiles, outlier spikes, texture-vs-albedo tint mismatches,
/// flatness/plasticity, axis-grid periodicity, and shadow casting; writes crop sheet only when needed.
EyeDiagnoseResult diagnoseScene(scene::SceneGraph& graph, const EyeDiagnoseOptions& opts = {});

}  // namespace bro::eye

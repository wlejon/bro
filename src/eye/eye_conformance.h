#pragma once

#include "eye/eye_types.h"

#include <string>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeConformancePairSpec {
    std::string subject;
    std::vector<std::string> surfaces;
    double maxGap = 0.06;
    double minGap = -0.025;
    int maxSamples = 256;
};

struct EyeConformanceOptions {
    std::vector<EyeConformancePairSpec> pairs;
    int sheetWidth = 768;
    int sheetHeight = 384;
    std::string path;
};

struct EyeConformancePairResult {
    std::string subject;
    int surfaceCount = 0;
    int sampleCount = 0;
    double minGap = 0.0;
    double p50Gap = 0.0;
    double p95Gap = 0.0;
    double maxGap = 0.0;
    double airGapFrac = 0.0;
    double penetratingFrac = 0.0;
    double worstPoint[3] = {0.0, 0.0, 0.0};
    bool ok = true;
};

struct EyeConformanceResult {
    bool ok = true;
    int width = 0;
    int height = 0;
    std::vector<EyeConformancePairResult> pairs;
    EyeImage visual;
    std::string visualPath;
};

/// Pure-geometry surface-hugging audit: measures signed closest distance from `subject` mesh vertices
/// to `surfaces` BVHs in world space. Zero images unless `opts.path` is explicitly provided.
EyeConformanceResult auditConformance(scene::SceneGraph& graph, const EyeConformanceOptions& opts);

}  // namespace bro::eye

#pragma once

#include "eye/eye_types.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeClearancePair {
    std::string nodeA;
    std::string nodeB;
    int hitTriangles = 0;
    int poseIndex = 0;
    double centroid[3] = {0.0, 0.0, 0.0};
    double overlapDepth = 0.0;
    bool dynamic = false;
};

struct EyeClearanceOptions {
    double eps = 0.003;
    int steps = 1;
    bool ignoreStatic = false;
    bool ignoreSameParent = false;
    std::vector<std::pair<std::string, std::string>> ignorePairs;
    std::string path;
    int maxPanelDim = 960;
};

struct EyeClearanceResult {
    bool ok = true;
    int posesChecked = 0;
    int totalMeshPairs = 0;
    int staticCollisionCount = 0;
    int dynamicCollisionCount = 0;
    int worstPoseIndex = 0;
    std::vector<EyeClearancePair> collisions;
    EyeImage visual;
    std::string visualPath;
};

/// Exact 3D triangle-triangle clearance sweep across one or more kinematic poses
/// (`stepFn(poseIdx, totalSteps)`). Optionally writes an annotated blueprint X-ray PNG.
EyeClearanceResult analyzeClearance(scene::SceneGraph& graph,
                                    const EyeClearanceOptions& opts = {},
                                    const std::function<void(int, int)>& stepFn = {});

}  // namespace bro::eye

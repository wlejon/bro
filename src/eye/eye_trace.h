#pragma once

#include "eye/eye_types.h"

#include <functional>
#include <string>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeTraceOptions {
    int steps = 24;
    double waterY = 0.0;
    double minTravel = 0.03;
    std::string filter;
    int sheetWidth = 768;
    int sheetHeight = 432;
    std::string path;
};

struct EyeTraceTrack {
    std::string name;
    double startPos[3] = {0.0, 0.0, 0.0};
    double endPos[3] = {0.0, 0.0, 0.0};
    double minY = 0.0;
    double maxY = 0.0;
    double pathLength = 0.0;
    double maxStepJump = 0.0;
    double maxVelJump = 0.0;
    double maxRotSnapDeg = 0.0;
    int teleportCount = 0;
    int snapCount = 0;
    bool closedLoop = false;
    bool touchesWater = false;
    std::string nearestStartStatic;
    double nearestStartDist = 0.0;
};

struct EyeTraceResult {
    bool ok = true;
    int steps = 0;
    int trackCount = 0;
    int width = 0;
    int height = 0;
    std::vector<EyeTraceTrack> tracks;
    EyeImage visual;
    std::string visualPath;
};

/// Step through cyclic/animated poses via `stepFn(k, steps)`, record 3D world-space paths
/// of moving scene nodes, detect mid-air loops/teleports/water touchdowns, and optionally render
/// an exact native-tile trajectory overlay PNG.
EyeTraceResult traceSceneMotion(scene::SceneGraph& graph, const EyeTraceOptions& opts,
                                const std::function<void(int, int)>& stepFn);

}  // namespace bro::eye

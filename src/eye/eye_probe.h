#pragma once

#include "eye/eye_types.h"

#include <string>
#include <utility>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeProbePoint {
    double x = 0.0;
    double y = 0.0;
    bool normalized = true;
    std::string label;
};

struct EyeProbeOptions {
    std::vector<EyeProbePoint> points;
    int gridCols = 5;
    int gridRows = 3;
    bool render = true;
    std::vector<std::pair<std::string, std::string>> pairs;
    std::string path;
    int maxPanelDim = 960;
};

struct EyeProbeSample {
    int index = 0;
    std::string label;
    int px = 0;
    int py = 0;
    double normX = 0.0;
    double normY = 0.0;
    bool hit = false;
    std::string nodeName;
    uint64_t nodeId = 0;
    double point[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 0.0, 0.0};
    double distance = 0.0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    std::string hex;
    double L = 0.0;
    double C = 0.0;
    double h = 0.0;
};

struct EyeProbeDistance {
    std::string a;
    std::string b;
    double distance = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    double dz = 0.0;
};

struct EyeProbeResult {
    int width = 0;
    int height = 0;
    std::vector<EyeProbeSample> samples;
    std::vector<EyeProbeDistance> distances;
    EyeImage visual;
    std::string visualPath;
};

/// Precision 3D spatial calipers: sample rendered pixels + BVH world hits
/// + pairwise 3D clearances across explicit or gridded screen coordinates.
EyeProbeResult probeScene(scene::SceneGraph& graph, const EyeProbeOptions& opts = {});

}  // namespace bro::eye

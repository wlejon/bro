#pragma once

#include "eye/eye_types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeStructureIssue {
    std::string kind;       // "inverted-winding" | "floating" | "overhang" | "unmeshed-gap" | "floor-separation" | "doorway-blocked" | "doorway-narrow" | "corridor-blocked" | "corridor-narrow" | "stair-gap" | "stair-step-too-high" | "stair-blocked" | "stair-narrow" | "stair-low-headroom" | "low-headroom"
    std::string severity;   // "error" | "warning"
    std::string nodeA;
    std::string nodeB;
    double value = 0.0;
    double point[3] = {0.0, 0.0, 0.0};
    double radius = 0.25;
    std::string message;
};

struct EyeStructurePairGap {
    std::string nodeA;
    std::string nodeB;
    double gap = 0.0;
    double pointA[3] = {0.0, 0.0, 0.0};
    double pointB[3] = {0.0, 0.0, 0.0};
};

struct EyeStructureFloorSpec {
    double y = 0.0;
    double tolerance = 0.05;
    std::string name;
};

struct EyeStructureDoorwaySpec {
    std::string name;
    double position[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 0.0, 1.0};
    double width = 0.9;
    double height = 2.1;
    double depth = 0.8;
};

struct EyeStructureCorridorSpec {
    std::string name;
    double start[3] = {0.0, 0.0, 0.0};
    double end[3] = {0.0, 0.0, 0.0};
    double width = 0.0;
    double height = 0.0;
};

struct EyeStructureStairSpec {
    std::string name;
    double start[3] = {0.0, 0.0, 0.0};
    double end[3] = {0.0, 0.0, 0.0};
    double width = 0.0;
    double headroom = 0.0;
};

struct EyeStructureInteriorWalkOptions {
    bool enabled = false;
    double agentRadius = 0.35;
    double agentHeight = 1.85;
    double stepUp = 0.40;
    double minHeadroom = 1.95;
    double maxFloorGap = 0.12;
    double sampleStep = 0.25;
    std::vector<std::string> ignoreNodes;
    std::vector<EyeStructureDoorwaySpec> doorways;
    std::vector<EyeStructureCorridorSpec> corridors;
    std::vector<EyeStructureStairSpec> stairs;
};

struct EyeStructureFloorSummary {
    std::string name;
    double y = 0.0;
    double tolerance = 0.05;
    int slabCount = 0;
    int entryCount = 0;
    int floatingCount = 0;
    int overhangCount = 0;
};

struct EyeStructureWalkResult {
    bool checked = false;
    int doorwaysChecked = 0;
    int doorwaysBlocked = 0;
    int corridorsChecked = 0;
    int corridorsBlocked = 0;
    int stairsChecked = 0;
    int stairsBroken = 0;
    int headroomIssues = 0;
    int walkIssueCount = 0;
};

struct EyeGroundHeightGrid {
    double originX = 0.0;
    double originZ = 0.0;
    double step = 1.0;
    int cols = 0;
    int rows = 0;
    std::vector<float> heights;
};

struct EyeStructureOptions {
    double contactTol = 0.028;
    double overhangTol = 0.25;
    double groundY = 0.0;
    EyeGroundHeightGrid groundGrid;
    bool checkIslands = true;
    std::vector<std::string> ignoreFloating;
    std::vector<std::pair<std::string, std::string>> watchPairs;
    std::string path;
    int maxPanelDim = 420;
    int maxZoomPanels = 6;

    bool hasBounds = false;
    double boundsMin[3] = {0.0, 0.0, 0.0};
    double boundsMax[3] = {0.0, 0.0, 0.0};
    bool hasNear = false;
    double nearCenter[3] = {0.0, 0.0, 0.0};
    double nearRadius = 0.0;
    int maxInstances = 4096;

    std::vector<EyeStructureFloorSpec> floors;
    bool autoDetectFloors = false;
    double minFloorSeparation = 1.8;

    EyeStructureInteriorWalkOptions interiorWalk;

    double sampleGroundY(double x, double z) const {
        if (groundGrid.cols < 2 || groundGrid.rows < 2 || !(groundGrid.step > 0.0) ||
            groundGrid.heights.size() < static_cast<size_t>(groundGrid.cols) * static_cast<size_t>(groundGrid.rows)) {
            return groundY;
        }
        const double fx = (x - groundGrid.originX) / groundGrid.step;
        const double fz = (z - groundGrid.originZ) / groundGrid.step;
        if (!std::isfinite(fx) || !std::isfinite(fz) ||
            fx < 0.0 || fz < 0.0 ||
            fx > static_cast<double>(groundGrid.cols - 1) ||
            fz > static_cast<double>(groundGrid.rows - 1)) {
            return groundY;
        }
        const int c0 = std::min(static_cast<int>(std::floor(fx)), groundGrid.cols - 2);
        const int r0 = std::min(static_cast<int>(std::floor(fz)), groundGrid.rows - 2);
        const double tx = fx - static_cast<double>(c0);
        const double tz = fz - static_cast<double>(r0);
        const size_t stride = static_cast<size_t>(groundGrid.cols);
        const size_t idx00 = static_cast<size_t>(r0) * stride + static_cast<size_t>(c0);
        const double h00 = groundGrid.heights[idx00];
        const double h10 = groundGrid.heights[idx00 + 1];
        const double h01 = groundGrid.heights[idx00 + stride];
        const double h11 = groundGrid.heights[idx00 + stride + 1];
        const double hx0 = h00 + (h10 - h00) * tx;
        const double hx1 = h01 + (h11 - h01) * tx;
        const double h = hx0 + (hx1 - hx0) * tz;
        return std::isfinite(h) ? h : groundY;
    }
};

struct EyeStructureResult {
    bool ok = true;
    int meshCount = 0;
    int instancedMeshCount = 0;
    int invertedCount = 0;
    int floatingCount = 0;
    int invertedIslandCount = 0;
    int floatingIslandCount = 0;
    int overhangCount = 0;
    int gapCount = 0;
    int floorDefectCount = 0;
    std::vector<EyeStructureIssue> issues;
    std::vector<EyeStructurePairGap> watchedGaps;
    std::vector<EyeStructureFloorSummary> floors;
    EyeStructureWalkResult interiorWalk;
    EyeImage visual;
    std::string visualPath;
};

/// Structural 3D assembly audit: detects inverted face winding, disconnected/floating parts,
/// overhanging base footings, and unmeshed watch-pair gaps, optionally writing an auto-zoomed macro defect sheet.
EyeStructureResult analyzeStructure(scene::SceneGraph& graph, const EyeStructureOptions& opts = {});

}  // namespace bro::eye

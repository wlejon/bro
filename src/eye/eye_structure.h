#pragma once

#include "eye/eye_types.h"

#include <string>
#include <utility>
#include <vector>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

struct EyeStructureIssue {
    std::string kind;       // "inverted-winding" | "floating" | "overhang" | "unmeshed-gap"
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

struct EyeStructureOptions {
    double contactTol = 0.028;
    double overhangTol = 0.25;
    double groundY = 0.0;
    std::vector<std::string> ignoreFloating;
    std::vector<std::pair<std::string, std::string>> watchPairs;
    std::string path;
    int maxPanelDim = 420;
    int maxZoomPanels = 6;
};

struct EyeStructureResult {
    bool ok = true;
    int meshCount = 0;
    int invertedCount = 0;
    int floatingCount = 0;
    int overhangCount = 0;
    int gapCount = 0;
    std::vector<EyeStructureIssue> issues;
    std::vector<EyeStructurePairGap> watchedGaps;
    EyeImage visual;
    std::string visualPath;
};

/// Structural 3D assembly audit: detects inverted face winding, disconnected/floating parts,
/// overhanging base footings, and unmeshed watch-pair gaps, optionally writing an auto-zoomed macro defect sheet.
EyeStructureResult analyzeStructure(scene::SceneGraph& graph, const EyeStructureOptions& opts = {});

}  // namespace bro::eye

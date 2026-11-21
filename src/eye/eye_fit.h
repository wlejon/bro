#pragma once

#include "eye/eye_types.h"

#include <functional>
#include <string>
#include <vector>

namespace bro::eye {

struct EyeFitParam {
    std::string name;
    double min = 0.0;
    double max = 1.0;
    double start = 0.5;
    bool hasStart = false;
    std::string scale = "linear";  // "linear" | "log"
};

struct EyeFitMeasureTarget {
    std::string name;
    double target = 0.0;
    double weight = 1.0;
    double scale = 0.0;  // 0 = auto default per metric
};

struct EyeFitOptions {
    std::vector<EyeFitParam> params;
    EyeImage reference;
    bool hasReference = false;
    double referenceWeight = 1.0;
    std::vector<EyeFitMeasureTarget> measures;
    double auditErrorWeight = 0.0;
    double auditWarningWeight = 0.0;
    double customScorerWeight = 1.0;
    int maxEvaluations = 120;
    double targetDistance = 0.01;
    uint64_t seed = 1;
};

struct EyeFitCandidateResult {
    EyeImage image;
    bool hasCustomScore = false;
    double customScore = 0.0;
    int auditErrors = 0;
    int auditWarnings = 0;
    bool hasAudit = false;
    bool ok = true;
};

struct EyeFitResult {
    bool ok = false;
    double distance = 0.0;
    std::vector<double> bestValues;
    int evaluations = 0;
    EyeMeasurement frame;
    EyeComparison comparison;
    bool hasComparison = false;
    std::vector<double> history;
};

/// Deterministic CMA-ES optimization over parameter tuples in unit-cube space.
EyeFitResult fitVisualParams(
    const EyeFitOptions& opts,
    const std::function<EyeFitCandidateResult(const std::vector<double>& values)>& evalFn,
    const std::function<void(int eval, double bestDist, const std::vector<double>& bestVals)>& progressFn = {});

}  // namespace bro::eye

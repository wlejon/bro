#pragma once

#include "eye/eye_motion.h"

#include <vector>

namespace bro::eye {

/// Builds stroboscopic chronophotograph (`"chrono"`), slit-scan X-T / Y-T
/// kymograph (`"kymograph"`), cadence ECG oscilloscope, endpoint/trail sheet
/// (`"sheet"`), or combined 4-panel `"board"` visual for `analyzeMotion`.
EyeVisualResult buildMotionVisual(const std::vector<EyeImageView>& rawFrames,
                                  const std::vector<EyeImage>& workFrames,
                                  const std::vector<double>& maxDeltaMap,
                                  int workW,
                                  int workH,
                                  int minBx,
                                  int minBy,
                                  int maxBx,
                                  int maxBy,
                                  const EyeMotionOptions& opts,
                                  EyeMotionResult& ioResult);

}  // namespace bro::eye

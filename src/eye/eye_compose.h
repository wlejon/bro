#pragma once

#include "eye/eye_types.h"

#include <vector>

namespace bro::eye {

/// Multi-scale center-surround Oklab + Sobel edge saliency field normalized to [0, 1].
void computeSaliencyField(EyeImageView img, int& outW, int& outH, std::vector<float>& outSal);

/// Macro-blurred Notan tonal mass bands (values 0..tones-1, where tones is 3 or 5).
void computeNotanField(EyeImageView img, int tones, int& outW, int& outH, std::vector<uint8_t>& outBands);

/// Deterministic visual composition analysis: saliency/focal hierarchy, Notan massing,
/// rest-vs-detail rhythm, and Oklab palette harmony.
EyeComposeResult analyzeComposition(EyeImageView img, const EyeComposeOptions& opts = {});

}  // namespace bro::eye

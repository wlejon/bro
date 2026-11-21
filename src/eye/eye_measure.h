#pragma once

#include "eye/eye_types.h"

namespace bro::eye {

/// Deterministic visual & perceptual analysis of an RGBA8 image.
EyeMeasurement measureImage(EyeImageView img, const EyeMeasureOptions& opts = {});

/// Deterministic comparison between a candidate image `a` and reference `b`.
EyeComparison compareImages(EyeImageView a, EyeImageView b, const EyeCompareOptions& opts = {});

/// Computes the centered 2D log-power FFT spectrum (size x size floats in 0..1)
/// used by the tiling detector and visualizer.
void computeFftPowerMap(EyeImageView img, int fftSize, std::vector<float>& outLogNorm, EyeTiling* outTiling = nullptr);

/// Resamples an RGBA8 image bilinearly to dstW x dstH.
EyeImage resampleRgba(EyeImageView src, int dstW, int dstH);

}  // namespace bro::eye

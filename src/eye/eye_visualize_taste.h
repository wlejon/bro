#pragma once

#include "eye/eye_types.h"

#include <vector>

namespace bro::eye {

/// Builds composition / sculptor visual diagnostic modes:
/// 'saliency', 'notan', 'rails', 'zebra', 'curvature'.
bool visualizeTasteMode(const std::string& mode,
                        const std::vector<EyeImageView>& images,
                        const EyeVisualizeOptions& opts,
                        EyeVisualResult& outResult);

}  // namespace bro::eye

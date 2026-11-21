#pragma once

#include "eye/eye_types.h"

#include <bromesh/mesh_data.h>

namespace bro::eye {

/// Rasterizes orthographic silhouettes around Y and extracts enclosed negative space,
/// convex hull solidity, narrow pinch gaps, and smooth arc-length contour curvature kappa(s).
std::vector<EyeSilhouetteView> analyzeMeshSilhouettes(const bromesh::MeshData& mesh,
                                                      const std::array<double, 3>& center,
                                                      double radius,
                                                      const EyeFormOptions& opts);

}  // namespace bro::eye

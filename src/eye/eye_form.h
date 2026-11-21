#pragma once

#include "eye/eye_types.h"

#include <bromesh/mesh_data.h>

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

/// Geometric & differential-geometry form audit of a 3D mesh:
/// intrinsic Gaussian/mean curvature, G1 dihedral creases, curvature pinches & flat spots,
/// divergence volume & CoM, base support-polygon plumb-line stability, X-symmetry, slice profiles,
/// and multi-angle turntable silhouettes + contour curvature kappa(s).
EyeFormResult analyzeMeshForm(const bromesh::MeshData& mesh, const EyeFormOptions& opts = {});

/// Audits the focus node (or all non-ground subject meshes) inside a live 3D SceneGraph,
/// adding depth-masked lighting modeling analysis (highlight/halftone/core-shadow/reflected/warm-cool).
EyeFormResult analyzeSceneForm(scene::SceneGraph& graph, const EyeFormOptions& opts = {});

}  // namespace bro::eye

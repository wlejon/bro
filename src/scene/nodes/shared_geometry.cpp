#include "scene/nodes/shared_geometry.h"

#include "scene/texture_source.h"

#include <bromesh/analysis/bbox.h>
#include <bromesh/manipulation/normals.h>

#include <algorithm>
#include <cmath>

namespace bro::scene {

std::shared_ptr<const SharedGeometry> SharedGeometry::make(bromesh::MeshData&& mesh) {
    auto g = std::make_shared<SharedGeometry>();
    g->mesh = std::move(mesh);
    // As MeshNode::setMesh: tangents for normal mapping where the mesh can have them.
    if (g->mesh.hasUVs() && g->mesh.hasNormals() && !g->mesh.hasTangents()) bromesh::generateTangents(g->mesh);
    if (!g->mesh.empty()) g->bounds = bromesh::computeBBox(g->mesh);
    for (size_t i = 3; i < g->mesh.vegetation.size(); i += 16)
        if (std::isfinite(g->mesh.vegetation[i])) g->vegetationHeight = std::max(g->vegetationHeight, g->mesh.vegetation[i]);
    g->generation = nextResourceGeneration();
    return g;
}

}  // namespace bro::scene

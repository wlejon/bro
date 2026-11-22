#pragma once

#include "eye/eye_structure.h"
#include "eye/eye_structure_collect.h"
#include "eye/eye_structure_floors.h"

#include <vector>

namespace bro::eye {

/// Analyze interior walkability (doorways, corridors, stairs, floor-to-floor headroom)
/// using triangle-accurate MeshBVH::queryBox + SAT triangle-box overlap and MeshBVH::raycast.
void analyzeInteriorWalk(const std::vector<StructureMeshEntry>& entries,
                         const std::vector<ResolvedFloor>& resolvedFloors,
                         const EyeStructureOptions& opts,
                         EyeStructureResult& res);

}  // namespace bro::eye

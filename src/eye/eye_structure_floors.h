#pragma once

#include "eye/eye_structure.h"
#include "eye/eye_structure_collect.h"

#include <limits>
#include <string>
#include <vector>

namespace bro::eye {

struct ResolvedFloor {
    std::string name;
    double y = 0.0;
    double tolerance = 0.05;
    double minSlabY = std::numeric_limits<double>::infinity();
    int slabCount = 0;
    int entryCount = 0;
    int floatingCount = 0;
    int overhangCount = 0;
};

/// Resolve explicit / auto-detected floor slabs, check slab separation,
/// tag floor anchor slabs, and assign floorIndex to entries.
void prepareFloors(std::vector<StructureMeshEntry>& entries,
                   const EyeStructureOptions& opts,
                   std::vector<ResolvedFloor>& outFloors,
                   EyeStructureResult& res);

/// Audit per-floor resting prop/furniture footings against floor slabs below them.
/// Sets outHandled[i] = true for entries checked here so legacy Check C won't double-report.
void auditFloorFootings(const std::vector<StructureMeshEntry>& entries,
                        const EyeStructureOptions& opts,
                        const std::vector<bool>& reached,
                        double overhangTol,
                        std::vector<ResolvedFloor>& floors,
                        EyeStructureResult& res,
                        std::vector<bool>& outHandled);

/// Copy resolved floor summaries into res.floors.
void finalizeFloorSummaries(const std::vector<ResolvedFloor>& floors,
                            EyeStructureResult& res);

}  // namespace bro::eye

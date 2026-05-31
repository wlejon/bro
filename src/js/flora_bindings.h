#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Bindings for broflora — ecosystem simulation (Makowski et al. 2019,
// "Synthetic Silviculture"). Exposed as `bro.flora.*`.
//
// Surface:
//   bro.flora.createWorld(opts) -> World
//     world.addPrototype(spec) -> int
//     world.addVoronoiSite(prototypeIndex, determinacy, apicalControl)
//     world.addPlant(spec) -> int
//     world.step(dt)
//     world.emitMesh(sides=6) -> Mesh
//     world.emitSegments() -> Array<{from, to, radius, depth, parent}>
//     world.emitFoliage()  -> Array<{mass, age01, vigor01, light01, lightExposure01, senescence01, isTerminal}>
//     world.emitBloomAnchors() -> Array<{position, normal, age01, vigor01, lightExposure01, senescence01}>
//     world.validate() -> string|null
//     get simTime, plantCount, prototypeCount
class FloraBindings {
public:
    static void install(JSContext* ctx);
};

} // namespace bro::js

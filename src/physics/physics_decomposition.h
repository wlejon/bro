#pragma once

#include "physics/physics_world.h"
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace bro::physics {

using JPH::RefConst;

RefConst<JPH::Shape> buildDecomposedMeshShape(const BodyOptions& opts);

} // namespace bro::physics

#pragma once

extern "C" {
#include "quickjs.h"
}

#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

namespace bro::physics { class PhysicsWorld; }

namespace bro::js {

class PhysicsBindings {
public:
    static void install(JSContext* ctx, physics::PhysicsWorld* world);
    static void cleanup(JSContext* ctx);

    /// Map JS body tag → Jolt BodyID. Returns invalid BodyID if tag is unknown.
    static JPH::BodyID bodyIdForTag(int32_t tag);
    /// Map Jolt BodyID → JS body tag. Returns -1 if untracked.
    static int32_t tagForBodyId(JPH::BodyID id);
};

} // namespace bro::js

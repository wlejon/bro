#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::physics { class PhysicsWorld; }

namespace bro::js {

class PhysicsBindings {
public:
    static void install(JSContext* ctx, physics::PhysicsWorld* world);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js

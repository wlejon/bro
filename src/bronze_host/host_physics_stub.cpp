#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

void installPhysicsGlobals() {
    ev::registerGlobal("Physics", makeUnavailableNamespace("Physics", "BRO_WITH_PHYSICS"));
    ev::registerGlobal("PhysicsCharacter", makeUnavailableNamespace("PhysicsCharacter", "BRO_WITH_PHYSICS"));
    ev::registerGlobal("PhysicsSoftBody", makeUnavailableNamespace("PhysicsSoftBody", "BRO_WITH_PHYSICS"));
}

void drainPhysicsContactEvents() {
}

}  // namespace bro::bronze_host

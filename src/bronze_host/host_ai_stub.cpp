#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

void installAIGlobals() {
    ev::registerGlobal("AI", makeUnavailableNamespace("AI", "BRO_WITH_GAMEAI"));
    ev::registerGlobal("AINavGrid", makeUnavailableNamespace("AINavGrid", "BRO_WITH_GAMEAI"));
    ev::registerGlobal("AINavMesh", makeUnavailableNamespace("AINavMesh", "BRO_WITH_GAMEAI"));
    ev::registerGlobal("AIAgent", makeUnavailableNamespace("AIAgent", "BRO_WITH_GAMEAI"));
    ev::registerGlobal("AIUnit", makeUnavailableNamespace("AIUnit", "BRO_WITH_GAMEAI"));
    ev::registerGlobal("AIHexNav", makeUnavailableNamespace("AIHexNav", "BRO_WITH_GAMEAI"));
    ev::registerGlobal("AIWorld", makeUnavailableNamespace("AIWorld", "BRO_WITH_GAMEAI"));
}

Value makeBroAiValue() {
    return makeUnavailableNamespace("ai", "BRO_WITH_GAMEAI");
}

}  // namespace bro::bronze_host

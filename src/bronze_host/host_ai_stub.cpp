#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

void installAIGlobals() {
}

Value makeBroAiValue() {
    return makeUnavailableNamespace("ai", "BRO_WITH_GAMEAI");
}

}  // namespace bro::bronze_host

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

void installAIGlobals() {
}

Value makeBroAiValue() {
    ObjectBuilder ai;
    return ai.get();
}

}  // namespace bro::bronze_host

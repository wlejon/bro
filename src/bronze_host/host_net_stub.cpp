#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

Value makeBroNetValue() {
    return makeUnavailableNamespace("net", "BRO_WITH_NET");
}

void installNetGlobals() {
}

void drainNetEvents() {
}

void installNetSync(engine::Engine*) {
}

}  // namespace bro::bronze_host

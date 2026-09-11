#include "bronze_host/host_interp.h"

namespace bro::bronze_host {

void installInterpBridge(engine::Engine&) {}

Value bridgeJsGlobal(const char*) {
    return ev::undefined();
}

void sweepInterpBridge() {}

void resetInterpBridge() {}

}  // namespace bro::bronze_host

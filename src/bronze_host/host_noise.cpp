#include "bronze_host/host_globals_internal.h"
#include <api/api.h>

namespace bro::bronze_host {

void installNoiseGlobals() {
#ifdef BROKIT_HAS_NOISE
    brokit::api::installNoise();
#endif
}

} // namespace bro::bronze_host

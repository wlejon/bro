// `__bro_native.time` — the C entry points behind bro.time (docs/time-api.js):
// the engine's one scaled clock, its scale and its pause flag. The public
// accessors are js/bro_core.js's; each reads or writes exactly one of these.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "natives/time/native_time_decl.h"

extern "C" {

double bro_time_scale_get(void) {
    auto* eng = bro::bronze_host::hostEngine();
    return eng ? eng->timeScale() : 1.0;
}

void bro_time_scale_set(double v) {
    if (auto* eng = bro::bronze_host::hostEngine()) eng->setTimeScale(v);
}

bool bro_time_paused_get(void) {
    auto* eng = bro::bronze_host::hostEngine();
    return eng && eng->timePaused();
}

void bro_time_paused_set(bool v) {
    if (auto* eng = bro::bronze_host::hostEngine()) eng->setTimePaused(v);
}

double bro_time_now_get(void) {
    auto* eng = bro::bronze_host::hostEngine();
    return eng ? eng->timeNowMs() : bro::bronze_host::hostClockMs();
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_time(std::string* error);

bool registerTimeNatives(std::string* error) {
    return registerNatives_time(error);
}

}  // namespace bro::bronze_host

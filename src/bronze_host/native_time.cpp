// `__bro_native.time` — the C entry points behind bro.time (docs/time-api.js):
// the engine's one scaled clock, its scale and its pause flag. The public
// accessors are js/bro_core.js's; each reads or writes exactly one of these.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"

namespace bro::bronze_host {

namespace {

double scaleGet() {
    auto* eng = hostEngine();
    return eng ? eng->timeScale() : 1.0;
}

// Engine::setTimeScale clamps to [0, 100] and ignores a non-finite value,
// which is the documented contract; nothing is repeated here.
void scaleSet(double v) {
    if (auto* eng = hostEngine()) eng->setTimeScale(v);
}

bool pausedGet() {
    auto* eng = hostEngine();
    return eng && eng->timePaused();
}

void pausedSet(bool v) {
    if (auto* eng = hostEngine()) eng->setTimePaused(v);
}

double nowGet() {
    auto* eng = hostEngine();
    return eng ? eng->timeNowMs() : hostClockMs();
}

}  // namespace

bool registerTimeNatives(std::string* error) {
    using namespace natives;
    return getter("__bro_native.time.scale", reinterpret_cast<void*>(&scaleGet), "f64", error) &&
           setter("__bro_native.time.scale", reinterpret_cast<void*>(&scaleSet), "f64", error) &&
           getter("__bro_native.time.paused", reinterpret_cast<void*>(&pausedGet), "bool", error) &&
           setter("__bro_native.time.paused", reinterpret_cast<void*>(&pausedSet), "bool", error) &&
           getter("__bro_native.time.now", reinterpret_cast<void*>(&nowGet), "f64", error);
}

}  // namespace bro::bronze_host

// `__bro_native.server` — the C entry points behind bro.server
// (docs/server-api.js): the tick rate, uptime and stop request of the loop
// the calling realm runs in. On the main thread that is the engine's server
// loop (bro-server; a windowed bro answers the same members for a script
// that reads them). On a Worker's thread it is the worker's own loop, which
// the worker describes with setWorkerServerControl before its script runs
// (host_worker.cpp) — the launcher hosts a game's server.js in a Worker and
// that script calls bro.server.stop() to end itself.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "natives/server/native_server_decl.h"
#include "util/log.h"

namespace {

// Per thread: a Worker sets its own, the main thread never does.
thread_local const bro::bronze_host::WorkerServerControl* t_workerControl = nullptr;

double clampTickRate(double hz) {
    if (!(hz >= 1.0)) hz = 1.0;
    if (hz > 1000.0) hz = 1000.0;
    return hz;
}

}  // namespace

extern "C" {

double bro_server_tickrate_get(void) {
    if (t_workerControl && t_workerControl->tickRate) return t_workerControl->tickRate();
    auto* eng = bro::bronze_host::hostEngine();
    return eng ? eng->serverTickRate() : 60.0;
}

void bro_server_tickrate_set(double v) {
    const double hz = clampTickRate(v);
    if (t_workerControl && t_workerControl->setTickRate) {
        t_workerControl->setTickRate(hz);
        LOG_INFO("[server] Tick rate set to %.0f Hz (worker)", hz);
        return;
    }
    if (auto* eng = bro::bronze_host::hostEngine()) {
        eng->setServerTickRate(hz);
        LOG_INFO("[server] Tick rate set to %.0f Hz", hz);
    }
}

double bro_server_uptime_get(void) {
    if (t_workerControl && t_workerControl->uptimeSec) return t_workerControl->uptimeSec();
    auto* eng = bro::bronze_host::hostEngine();
    return eng ? eng->serverUptime() : 0.0;
}

void bro_server_stop(void) {
    if (t_workerControl && t_workerControl->stop) {
        t_workerControl->stop();
        LOG_INFO("[server] Stop requested (worker)");
        return;
    }
    if (auto* eng = bro::bronze_host::hostEngine()) {
        eng->requestServerStop();
        LOG_INFO("[server] Stop requested");
    }
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_server(std::string* error);

bool registerServerNatives(std::string* error) {
    return registerNatives_server(error);
}

void setWorkerServerControl(const WorkerServerControl* control) {
    t_workerControl = control;
}

}  // namespace bro::bronze_host

#include "bronze_host/host_telemetry.h"

#include "embed/embed.h"

namespace bro::bronze_host {

HostTelemetry getHostTelemetry() {
    auto bz = bronze::embed::getRuntimeTelemetry();
    HostTelemetry tel;
    tel.heapUsedBytes = bz.heapUsedBytes;
    tel.heapCommittedBytes = bz.heapCommittedBytes;
    tel.heapReservedBytes = bz.heapReservedBytes;
    tel.gcCollections = bz.gcCollections;
    tel.gcPauseNs = bz.gcPauseNs;
    tel.shapeTransitions = bz.shapeTransitions;
    return tel;
}

}  // namespace bro::bronze_host

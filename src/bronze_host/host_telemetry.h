#pragma once

#include <cstddef>
#include <cstdint>

namespace bro::bronze_host {

struct HostTelemetry {
    size_t heapUsedBytes{0};
    size_t heapCommittedBytes{0};
    size_t heapReservedBytes{0};
    uint64_t gcCollections{0};
    uint64_t gcPauseNs{0};
    uint64_t shapeTransitions{0};
};

HostTelemetry getHostTelemetry();

}  // namespace bro::bronze_host

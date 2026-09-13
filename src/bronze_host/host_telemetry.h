#pragma once

#include <cstddef>
#include <cstdint>

namespace bro::dom {
class Document;
}

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

void updatePerfDocument(dom::Document* doc, const HostTelemetry& tel,
                        double fps, double frameMs, double jsMs, double layoutMs,
                        double rasterMs, double gpuMs, double drawMs,
                        int vpW, int vpH);

}  // namespace bro::bronze_host

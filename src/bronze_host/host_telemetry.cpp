#include "bronze_host/host_telemetry.h"

#include "dom/document.h"
#include "dom/element.h"
#include "embed/embed.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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

void updatePerfDocument(dom::Document* doc, const HostTelemetry& tel,
                        double fps, double frameMs, double jsMs, double layoutMs,
                        double rasterMs, double gpuMs, double drawMs,
                        int vpW, int vpH) {
    if (!doc) return;

    auto setText = [doc](const std::string& id, const std::string& text) {
        if (auto* el = doc->getElementById(id)) {
            el->setTextContent(text);
        }
    };

    auto setBar = [doc](const std::string& barId, const std::string& msId, double ms) {
        constexpr double kMaxMs = 8.0;
        double pct = std::min(100.0, (ms / kMaxMs) * 100.0);
        int px = std::max(1, static_cast<int>(std::round(pct * 1.8)));
        if (auto* bar = doc->getElementById(barId)) {
            bar->style().setProperty("width", std::to_string(px) + "px");
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2fms", ms);
        if (auto* lbl = doc->getElementById(msId)) {
            lbl->setTextContent(buf);
        }
    };

    // Overall stats
    setText("fps-value", std::to_string(static_cast<int>(std::round(fps))));
    char frameBuf[32];
    std::snprintf(frameBuf, sizeof(frameBuf), "%.1fms", frameMs);
    setText("frame-value", frameBuf);
    setText("viewport-value", std::to_string(vpW) + "x" + std::to_string(vpH));

    // Phase timings
    setBar("bar-js", "ms-js", jsMs);
    setBar("bar-layout", "ms-layout", layoutMs);
    setBar("bar-raster", "ms-raster", rasterMs);
    setBar("bar-gpu", "ms-gpu", gpuMs);
    setBar("bar-draw", "ms-draw", drawMs);

    // Bronze runtime telemetry
    char heapBuf[32];
    double heapMb = static_cast<double>(tel.heapUsedBytes) / (1024.0 * 1024.0);
    std::snprintf(heapBuf, sizeof(heapBuf), "%.1f MB", heapMb);
    setText("gc-heap-value", heapBuf);

    setText("gc-count-value", std::to_string(tel.gcCollections));

    char pauseBuf[32];
    double pauseMs = static_cast<double>(tel.gcPauseNs) / 1000000.0;
    std::snprintf(pauseBuf, sizeof(pauseBuf), "%.2fms", pauseMs);
    setText("gc-pause-value", pauseBuf);

    setText("shape-count-value", std::to_string(tel.shapeTransitions));
}

}  // namespace bro::bronze_host

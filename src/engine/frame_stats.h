#pragma once

// The 500 ms rolling frame statistics the perf HUD shows: frames per second,
// mean wall time per frame, and the mean per-phase times. The frame loop adds
// each phase's time to its accumulator as the phase finishes and closes the
// frame with addFrame(); every 500 ms of frames the accumulators roll up into
// the means Engine::perf*() report.

namespace bro::engine {

struct FrameStats {
    // Rolled-up means, as of the last completed 500 ms window.
    double statsFps = 0.0;
    double statsFrameTimeMs = 0.0;
    double phaseJsMs = 0.0;
    double phaseLayoutMs = 0.0;
    double phaseRasterMs = 0.0;
    double phaseGpuMs = 0.0;
    double phaseGlStateMs = 0.0;
    double phaseDrawMs = 0.0;
    double phaseUploadMs = 0.0;

    // The window being accumulated.
    double statsAccumMs = 0.0;
    int statsFrameCount = 0;
    double statsMinFrameMs = 999.0;
    double statsMaxFrameMs = 0.0;
    double totalFrameMs = 0.0;
    double accumJsMs = 0.0;
    double accumLayoutMs = 0.0;
    double accumRasterMs = 0.0;
    double accumGpuMs = 0.0;
    double accumGlStateMs = 0.0;
    double accumDrawMs = 0.0;
    double accumUploadMs = 0.0;

    // Counts one frame of `frameMs` wall time. Returns true when that frame
    // closed a window and the means were refreshed.
    bool addFrame(double frameMs) {
        totalFrameMs = frameMs;
        statsAccumMs += frameMs;
        statsFrameCount++;
        if (frameMs < statsMinFrameMs) statsMinFrameMs = frameMs;
        if (frameMs > statsMaxFrameMs) statsMaxFrameMs = frameMs;
        if (statsAccumMs < 500.0) return false;
        statsFps = statsFrameCount / (statsAccumMs / 1000.0);
        statsFrameTimeMs = statsAccumMs / statsFrameCount;
        double n = statsFrameCount;
        phaseJsMs      = accumJsMs      / n;
        phaseLayoutMs  = accumLayoutMs  / n;
        phaseRasterMs  = accumRasterMs  / n;
        phaseGpuMs     = accumGpuMs     / n;
        phaseGlStateMs = accumGlStateMs / n;
        phaseDrawMs    = accumDrawMs    / n;
        phaseUploadMs  = accumUploadMs  / n;
        accumJsMs = accumLayoutMs = accumRasterMs = accumGpuMs = accumGlStateMs = 0.0;
        accumDrawMs = accumUploadMs = 0.0;
        statsAccumMs = 0.0;
        statsFrameCount = 0;
        statsMinFrameMs = 999.0;
        statsMaxFrameMs = 0.0;
        return true;
    }
};

} // namespace bro::engine

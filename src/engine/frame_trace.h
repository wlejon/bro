#pragma once

// The frame flight recorder: one record per trip round the frame loop, kept
// in a ring (about two minutes at 60 Hz), always on. A tool that wants to
// know why an animation stuttered asks for the last few seconds after the
// fact (bro-ctl trace) instead of having had to arm something beforehand.
//
// A record says where the frame's wall time went (input, JS, waiting on the
// layout thread, recording, compositing, the GPU wait, the commit and the
// wait for the flip), what the layout and raster threads did for it, whether
// it presented and on which vblank (the kernel's flip timestamp and vblank
// counter, under DRM), and which generation of content it showed: the
// animation time that content was laid out at. Those last two are what
// separate a frame that missed its vblank from one that presented on time
// but showed the same picture again.
//
// Written by the engine thread only, read by the control commands (which
// also run on the engine thread, between frames). Docs: docs/agent-control.md.

#include <cstdint>
#include <string>
#include <vector>

namespace bro::engine {

struct FrameRecord {
    uint64_t frame = 0;
    double startMs = 0.0;      // frame start, CLOCK_MONOTONIC ms (util::currentTimeMs)
    double endMs = 0.0;        // after the present and any pacing wait

    // Main-thread phases, ms.
    double eventsMs = 0.0;     // transition/animation events from the last layout pass (JS)
    double controlMs = 0.0;    // agent-control commands, and the input they inject (JS handlers)
    double miscMs = 0.0;       // source watcher, media/terminal/WebGL pumps, taking the raster result
    double inputMs = 0.0;      // seat, libinput dispatch (JS handlers), compositor clients
    double tickMs = 0.0;       // timers, rAF, observers (the frame seam's JS) + world tick
    double jsMs = 0.0;         // of tickMs: the frame callbacks alone
    double layoutWaitMs = 0.0; // blocked on the layout thread's pass
    double recordMs = 0.0;     // display-list record + layer bookkeeping
    double compositeMs = 0.0;  // canvas raster + composite recording (CPU side)
    double presentMs = 0.0;    // presentCurrentFrame: submit, GPU wait, commit, flip wait
    double gpuWaitMs = 0.0;    // of presentMs: CPU blocked on the composite's GPU ticket
    double flipWaitMs = 0.0;   // of presentMs: commit + waiting for the flip to land
    double pacingWaitMs = 0.0; // after the present: waiting for a flip event / frame cap

    // Layout thread, for a pass this frame waited on.
    double styleMs = 0.0;
    double layoutPassMs = 0.0;
    double animTickMs = 0.0;
    uint32_t activeAnimations = 0;  // elements with a running animation/transition
    uint32_t promotedElements = 0;

    // Raster thread, for a result consumed (shown) this frame.
    double rasterMs = 0.0;

    // Content shown: the generation of the layers composited, and the
    // animation clock they were laid out at.
    uint64_t contentGen = 0;
    double contentTimeMs = 0.0;

    // The flip, under DRM: the vblank it landed on.
    double vblankMs = 0.0;     // kernel timestamp, CLOCK_MONOTONIC ms; 0 = no flip seen
    uint32_t vblankSeq = 0;

    uint32_t inputEvents = 0;
    // Layout forced by script on the main thread (a geometry read after a
    // mutation): how many times, and the ms they took (inside control/input/tick).
    uint32_t forcedLayouts = 0;
    double forcedLayoutMs = 0.0;
    uint8_t presented = 0;     // 0 nothing, 1 composited, 2 client scanned out directly
    bool layoutRan = false;    // a layout pass was signalled this frame
    bool layoutPerformed = false;  // ... and it ran layoutTree (not style/paint only)
    bool recorded = false;     // the app's base display list was re-recorded
    bool rasterSignalled = false;
    bool rasterConsumed = false;
    bool animating = false;    // animations active at the end of the frame
};

struct FrameMark {
    double ms = 0.0;
    uint64_t frame = 0;
    std::string label;
};

class FrameTrace {
public:
    static constexpr size_t kCapacity = 8192;
    static constexpr size_t kMarkCapacity = 256;

    FrameTrace() : ring_(kCapacity) {}

    /// The record being filled for the frame now running.
    FrameRecord& current() { return cur_; }
    void begin(uint64_t frame, double startMs) {
        cur_ = FrameRecord{};
        cur_.frame = frame;
        cur_.startMs = startMs;
    }
    void commit() {
        ring_[head_ % kCapacity] = cur_;
        ++head_;
    }

    void mark(double ms, const std::string& label) {
        FrameMark m{ms, cur_.frame, label};
        if (marks_.size() < kMarkCapacity) marks_.push_back(std::move(m));
        else marks_[markHead_++ % kMarkCapacity] = std::move(m);
    }

    /// Committed records with startMs in [fromMs, toMs], oldest first.
    std::vector<FrameRecord> range(double fromMs, double toMs) const;
    std::vector<FrameMark> marks(double fromMs, double toMs) const;
    size_t size() const { return head_ < kCapacity ? head_ : kCapacity; }
    const FrameRecord* last() const { return head_ ? &ring_[(head_ - 1) % kCapacity] : nullptr; }

    /// The display's refresh period (ms), for the summary's vblank units.
    void setRefreshPeriodMs(double ms) { refreshMs_ = ms; }
    double refreshPeriodMs() const { return refreshMs_; }

    /// A windowed frame's presentation, reported by the window system after
    /// the fact (Wayland presentation-time): fills in that frame's vblank if
    /// its record is still in the ring, and becomes the newest presentation.
    void notePresentation(uint64_t frame, double vblankMs, uint64_t sequence, double refreshMs) {
        if (refreshMs > 0.0) refreshMs_ = refreshMs;
        if (vblankMs > lastPresentationMs_) lastPresentationMs_ = vblankMs;
        auto fill = [&](FrameRecord& r) {
            r.vblankMs = vblankMs;
            r.vblankSeq = static_cast<uint32_t>(sequence);
        };
        if (cur_.frame == frame) {
            fill(cur_);
            return;
        }
        for (size_t back = 1; back <= 16 && back <= head_; ++back) {
            FrameRecord& r = ring_[(head_ - back) % kCapacity];
            if (r.frame == frame) {
                fill(r);
                return;
            }
            if (r.frame < frame) return;
        }
    }
    /// When the newest reported presentation turned to light (CLOCK_MONOTONIC
    /// ms); 0 when none was reported.
    double lastPresentationMs() const { return lastPresentationMs_; }

    /// JSON: every record in the window, one object each, plus the marks.
    std::string toJson(double fromMs, double toMs) const;
    /// JSON: pacing over the window — presents, vblank gaps (in refresh
    /// periods), repeated content while animating, phase means/p95/max and
    /// the longest frames with their breakdown.
    std::string summaryJson(double fromMs, double toMs, int worst = 8) const;
    /// summaryJson as a short human/agent-readable text report.
    std::string summaryText(double fromMs, double toMs, int worst = 8) const;

private:
    std::vector<FrameRecord> ring_;
    size_t head_ = 0;
    FrameRecord cur_;
    std::vector<FrameMark> marks_;
    size_t markHead_ = 0;
    double refreshMs_ = 0.0;
    double lastPresentationMs_ = 0.0;
};

}  // namespace bro::engine

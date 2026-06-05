#pragma once

#include <atomic>
#include <bit>
#include <optional>

#include "engine/ui_layer.h"
#include "render/frame_worker.h"

namespace bro::engine {

/// Owns the double-buffered layer lists and the snapshot atomics that the
/// main thread hands to the raster worker each frame. Wraps a FrameWorker
/// for the actual signal/wait/fence handshake.
///
/// All access to the front buffer index goes through currentLayers() or
/// consumeIfReady(); there is no API that returns the bare index. This makes
/// the historical "stale frontLayers reference around the raster flip" bug
/// structurally impossible — the only path that flips front_ is consumeIfReady,
/// and it returns the new view atomically with the flip.
class FramePresenter {
public:
    /// Snapshot of the values the raster thread reads each frame. Bundled
    /// so signalRender writes them all in one place — there is no API for
    /// signaling without supplying a complete snapshot, so the pre-refactor
    /// "no-layout branch forgot to write insets" bug can't happen.
    struct Snapshot {
        int vpWidth = 0;
        int vpHeight = 0;
        int insetTop = 0;
        int insetRight = 0;
        int insetBottom = 0;
        float scrollY = 0.0f;
    };

    /// Stable view of the current front layer set. Returned by both
    /// currentLayers() and consumeIfReady(); the references stay valid for
    /// the rest of the frame as long as no further consumeIfReady() runs.
    struct LayerView {
        int frontIdx;
        std::vector<UILayer>& appLayers;
        std::vector<UILayer>& systemLayers;
    };

    // ---- main thread ----

    /// Hand the raster worker a snapshot of viewport / inset / scroll. The
    /// worker must be Idle (caller checks via isRasterIdle()).
    void signalRender(const Snapshot& s) {
        vpWidth_.store(s.vpWidth, std::memory_order_relaxed);
        vpHeight_.store(s.vpHeight, std::memory_order_relaxed);
        insetTop_.store(s.insetTop, std::memory_order_relaxed);
        insetRight_.store(s.insetRight, std::memory_order_relaxed);
        insetBottom_.store(s.insetBottom, std::memory_order_relaxed);
        scrollYBits_.store(std::bit_cast<uint32_t>(s.scrollY),
                            std::memory_order_relaxed);
        worker_.postRequest();
    }

    /// If the worker has new textures published, wait on its fence, flip the
    /// front buffer, and return the fresh view. Otherwise std::nullopt.
    std::optional<LayerView> consumeIfReady() {
        if (!worker_.isResultReady()) return std::nullopt;
        // Worker stored newFront_ before publishing the fence; tryClaimResult
        // synchronises both via the state release/acquire pair.
        worker_.tryClaimResult();
        int newFront = pendingFront_.load(std::memory_order_acquire);
        front_.store(newFront, std::memory_order_release);
        return makeView(newFront);
    }

    /// View into the current front buffer. Safe to call any time on the main
    /// thread; references remain valid until the next consumeIfReady().
    LayerView currentLayers() {
        return makeView(front_.load(std::memory_order_acquire));
    }

    bool isRasterIdle() const { return worker_.isIdle(); }
    bool isRasterBusyOrRequested() const { return worker_.isBusyOrRequested(); }

    /// Drop every reference to a CanvasScene that is about to be destroyed from
    /// the front (composable) buffer, so compositeLayers can't dereference a
    /// freed scene. A canvas layer recorded in an earlier frame still names the
    /// scene by raw pointer; if the scene's element is detached and the engine
    /// erases the scene this frame, the live front-buffer view would dangle.
    ///
    /// Only the front buffer needs scrubbing: the worker writes exclusively to
    /// the back buffer, and a stale back buffer is always rewritten by the
    /// worker (which skips detached scenes at record time) before it can be
    /// flipped to front. So the front buffer is the only buffer composite ever
    /// reads, and scrubbing it here races nothing on the worker thread.
    void forgetCanvasScene(const canvas::CanvasScene* scene) {
        int f = front_.load(std::memory_order_acquire);
        auto scrub = [scene](std::vector<UILayer>& layers) {
            for (auto& l : layers) {
                if (l.canvasScene == scene) {
                    l.canvasScene = nullptr;
                    l.texture = 0;
                }
            }
        };
        scrub(buffers_[f].appLayers);
        scrub(buffers_[f].systemLayers);
    }

    /// Drop every reference to a CanvasScene from BOTH layer buffers. Unlike
    /// forgetCanvasScene (front only), this also scrubs the back buffer, which
    /// the raster worker writes — so the caller MUST guarantee the worker is
    /// idle (e.g. at frame top after consumeIfReady). Used by the deferred
    /// canvas-scene free: a scene recorded into the back buffer this frame
    /// becomes the front buffer next frame, so its raw pointer can outlive a
    /// front-only scrub. We clear both buffers before the scene is destroyed.
    void forgetCanvasSceneAllBuffers(const canvas::CanvasScene* scene) {
        auto scrub = [scene](std::vector<UILayer>& layers) {
            for (auto& l : layers) {
                if (l.canvasScene == scene) {
                    l.canvasScene = nullptr;
                    l.texture = 0;
                }
            }
        };
        for (auto& buf : buffers_) {
            scrub(buf.appLayers);
            scrub(buf.systemLayers);
        }
    }

    // ---- worker (raster) thread ----

    /// Block until either a request arrives or shutdown is signaled.
    /// Returns false on shutdown.
    bool waitForRequest() { return worker_.waitForRequest(); }

    void markBusy() { worker_.markBusy(); }

    /// Read the snapshot the main thread published with signalRender.
    Snapshot loadSnapshot() const {
        Snapshot s;
        s.vpWidth = vpWidth_.load(std::memory_order_relaxed);
        s.vpHeight = vpHeight_.load(std::memory_order_relaxed);
        s.insetTop = insetTop_.load(std::memory_order_relaxed);
        s.insetRight = insetRight_.load(std::memory_order_relaxed);
        s.insetBottom = insetBottom_.load(std::memory_order_relaxed);
        s.scrollY = std::bit_cast<float>(
            scrollYBits_.load(std::memory_order_relaxed));
        return s;
    }

    /// The buffer the raster worker should draw into for the current request.
    /// Must be called between waitForRequest() returning true and publishResult().
    LayerBuffer& backBuffer() {
        int back = 1 - front_.load(std::memory_order_acquire);
        return buffers_[back];
    }

    int backIndex() const {
        return 1 - front_.load(std::memory_order_acquire);
    }

    /// Worker hands off: stage the new front index, then publish the fence
    /// via FrameWorker. Main side reads pendingFront_ inside consumeIfReady
    /// after observing the state release.
    void publishResult(GLsync fence) {
        int back = 1 - front_.load(std::memory_order_acquire);
        pendingFront_.store(back, std::memory_order_release);
        worker_.publishResult(fence);
    }

    // ---- lifecycle ----

    void postShutdown() { worker_.postShutdown(); }

    /// Block until the worker is back at Idle (drops a published result on
    /// the way through). Use before destructive operations like resizing
    /// surface pools.
    void waitUntilIdle() { worker_.waitUntilIdle(); }

private:
    LayerView makeView(int idx) {
        return LayerView{idx, buffers_[idx].appLayers, buffers_[idx].systemLayers};
    }

    LayerBuffer buffers_[2];
    std::atomic<int> front_{0};
    std::atomic<int> pendingFront_{0};

    // Snapshot atomics — main writes inside signalRender, worker reads inside
    // loadSnapshot. Ordering is established by the FrameWorker state transition.
    std::atomic<int> vpWidth_{0};
    std::atomic<int> vpHeight_{0};
    std::atomic<int> insetTop_{0};
    std::atomic<int> insetRight_{0};
    std::atomic<int> insetBottom_{0};
    std::atomic<uint32_t> scrollYBits_{0};

    render::FrameWorker worker_;
};

} // namespace bro::engine

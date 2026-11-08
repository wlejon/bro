#pragma once

// WebGLChannel: one WebGL context's link between its client (WebGLClient, on
// the JS thread) and the WebGL thread (WebGLThread) that owns its backend.
//
// The client is the ring's only producer; the WebGL thread is its only
// consumer and the only thread that ever touches the backend
// (vk::WebGLVkContext) or the replay's state. The backend is made on the
// WebGL thread itself (an Attach record), so its stream's owner is that
// thread, and destroyed there (Detach: context loss; Destroy: the context
// goes).
//
// What crosses back, without a lock:
//   - publications into the client's slots (webgl_client_async.h);
//   - syncsAnswered: the last round trip (SyncCall) answered;
//   - framesEnded and frontImages: the last frame end (FrameEnd) replayed,
//     and the image it left for the compositor;
//   - the ring's read position and replay.blobBytes (back-pressure);
//   - progress: notified after every batch replayed and every round trip
//     answered, which is what the client sleeps on when it waits for any of
//     the above.
// The Destroy acknowledgement is the control plane's: WebGLThread's mutex.

#include "render/layer_image.h"
#include "webgl/client/webgl_client_async.h"
#include "webgl/client/webgl_ring.h"
#include "webgl/client/webgl_wait.h"
#include "webgl/webgl_types.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace bro::render { class VulkanContext; }

namespace bro::webgl {

namespace vk {
class WebGLVkContext;
struct WebGLCaps;
}  // namespace vk

class WebGLThread;

/// A round trip's callback, run on the WebGL thread with the backend (null
/// when the context has none: attach failed).
using WebGLSyncFn = void (*)(void* ctx, vk::WebGLVkContext* gl);

class WebGLChannel {
public:
    WebGLChannel(render::VulkanContext& device, std::shared_ptr<const vk::WebGLCaps> caps,
                 const ContextAttributes& attributes);
    /// The WebGL thread has dropped the channel (WebGLThread::awaitDestroyed)
    /// or never had it; whatever the ring still holds is released.
    ~WebGLChannel();

    WebGLChannel(const WebGLChannel&) = delete;
    WebGLChannel& operator=(const WebGLChannel&) = delete;

    WebGLRing ring;
    WebGLReplayState replay;
    WebGLWaitWord progress;
    /// The sequence number of the last SyncCall answered.
    std::atomic<uint64_t> syncsAnswered{0};
    /// The sequence number of the last FrameEnd replayed, stored (release)
    /// after its image: frontImages[n % 2] is what frame end n answered.
    /// The client reads the slot of a frame end it has seen acknowledged,
    /// and encodes frame end n + 2 (the next writer of that slot) only after
    /// that read, so the two never overlap.
    std::atomic<uint64_t> framesEnded{0};
    render::LayerImage frontImages[2];
    /// The client read a query still pending: publish query status again
    /// even without commands.
    std::atomic<bool> pollRequested{false};

    // --- The WebGL thread's ---

    /// Replay what is published, then publish the replay's state. True if
    /// anything was done.
    bool service();
    /// Something for service() to do.
    bool hasWork() const {
        return ring.hasPublished() || pollRequested.load(std::memory_order_relaxed);
    }
    /// The Destroy record has replayed: nothing follows it.
    bool closing() const { return closing_; }

private:
    friend class WebGLThread;

    void replayRecord(uint16_t op, const uint8_t* rec);

    render::VulkanContext& device_;
    std::shared_ptr<const vk::WebGLCaps> caps_;
    ContextAttributes attributes_;
    std::unique_ptr<vk::WebGLVkContext> backend_;  // the WebGL thread's
    bool closing_ = false;                          // the WebGL thread's
    bool destroyed_ = false;                        // under WebGLThread's mutex
};

}  // namespace bro::webgl

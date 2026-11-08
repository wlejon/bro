// The WebGL thread's half of a channel: replaying its records onto the
// backend, and the channel's own records (webgl_cmd_list.h,
// BRO_WEBGL_CHANNEL_COMMANDS).

#include "webgl/client/webgl_channel.h"

#include "webgl/client/webgl_client.h"
#include "webgl/client/webgl_client_records.h"
#include "webgl/vulkan/webgl_vk_caps.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <exception>

namespace bro::webgl {

WebGLChannel::WebGLChannel(render::VulkanContext& device, std::shared_ptr<const vk::WebGLCaps> caps,
                           const ContextAttributes& attributes)
    : device_(device), caps_(std::move(caps)), attributes_(attributes) {}

WebGLChannel::~WebGLChannel() {
    // The WebGL thread is done with the channel: release what the records
    // left unreplayed still hold (only a channel never served has any).
    ring.setHooks(nullptr, nullptr, nullptr);
    ring.publish();
    ring.consume([this](Op op, const uint8_t* rec) {
        if (op != Op::Attach && op != Op::Detach && op != Op::SyncCall && op != Op::FrameEnd && op != Op::Destroy)
            replayWebGLCommand(nullptr, op, rec, replay);
    });
    replay.clear();
    backend_.reset();
}

bool WebGLChannel::service() {
    const bool polled = pollRequested.exchange(false, std::memory_order_acquire);
    const size_t n = ring.consume([this](Op op, const uint8_t* rec) { replayRecord(static_cast<uint16_t>(op), rec); });
    if (n == 0 && !polled) return false;
    // A batch's end: what it changed is published (query status included,
    // which is what a poll asks for).
    if (backend_) replay.publish(*backend_);
    progress.notify();
    return true;
}

void WebGLChannel::replayRecord(uint16_t opBits, const uint8_t* rec) {
    const Op op = static_cast<Op>(opBits);
    switch (op) {
        case Op::Attach:
            cmd::decodeArgs<int, int>(rec, [this](int width, int height) {
                replay.clear();
                backend_.reset();
                try {
                    backend_ = std::make_unique<vk::WebGLVkContext>(width, height, device_, attributes_, caps_);
                } catch (const std::exception& e) {
                    LOG_ERROR("WebGL: creating the context failed: %s", e.what());
                }
            });
            return;
        case Op::Detach:
            replay.clear();
            backend_.reset();
            return;
        case Op::SyncCall:
            cmd::decodeArgs<WebGLSyncFn, void*, uint64_t>(rec, [this](WebGLSyncFn fn, void* ctx, uint64_t seq) {
                try {
                    fn(ctx, backend_.get());
                } catch (const std::exception& e) {
                    LOG_ERROR("WebGL: a round trip failed: %s", e.what());
                }
                if (backend_) replay.publish(*backend_);
                syncsAnswered.store(seq, std::memory_order_release);
                progress.notify();
            });
            return;
        case Op::FrameEnd:
            cmd::decodeArgs<uint64_t>(rec, [this](uint64_t seq) {
                render::LayerImage img;
                try {
                    if (backend_) img = backend_->endFrame();
                } catch (const std::exception& e) {
                    LOG_ERROR("WebGL: ending a frame failed: %s", e.what());
                }
                frontImages[seq % 2] = img;
                if (backend_) replay.publish(*backend_);
                framesEnded.store(seq, std::memory_order_release);
                progress.notify();
            });
            return;
        case Op::Destroy:
            replay.clear();
            backend_.reset();
            closing_ = true;
            return;
        default:
            break;
    }
    try {
        replayWebGLCommand(backend_.get(), op, rec, replay);
    } catch (const std::exception& e) {
        LOG_ERROR("WebGL: replaying a command failed: %s", e.what());
    }
}

}  // namespace bro::webgl

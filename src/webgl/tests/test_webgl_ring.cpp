// bro_webgl_ring_test: WebGL's command ring between two threads, and the
// WebGL thread's lifecycle (webgl/client/webgl_thread.h): round trips behind
// a backlog, the blob budget, many contexts on one thread, teardown and
// context loss with commands pending.
//
// assert() is the check here, so it must survive a Release build: NDEBUG is
// undefined before any header can pull in <cassert>. Run under
// BRO_VK_VALIDATION=1 (tests/run_tests.sh does): a threading mistake on the
// device shows up as a validation error, which fails the run.
#undef NDEBUG

#include "webgl/client/webgl_encoder.h"
#include "webgl/client/webgl_ring.h"
#include "webgl/client/webgl_thread.h"
#include "webgl/client/webgl_wait.h"
#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/webgl2_context.h"
#include "render/vulkan_context.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace bro;
using namespace bro::webgl;

namespace {

// --- Test 1: the ring alone, one producer and one consumer thread ----------

struct RingPair {
    WebGLRing ring{64 * 1024};
    WebGLWaitWord progress;  // the consumer moved (the producer waits on it)
    WebGLWaitWord work;      // the producer published (the consumer sleeps on it)
    std::atomic<bool> done{false};
    uint64_t fullWaits = 0;  // the producer's
};

uint8_t patternByte(uint64_t seq, size_t i) { return static_cast<uint8_t>(seq * 31 + i * 7); }

void testRingStress() {
    std::cout << "[Test 1] ring stress: wrap, back-pressure, payloads... " << std::flush;
    RingPair p;
    p.ring.setHooks(
        [](void* o, uint64_t seen) {
            auto* rp = static_cast<RingPair*>(o);
            ++rp->fullWaits;
            rp->progress.wait([&] { return rp->ring.readPosition() != seen; }, std::chrono::microseconds(20));
        },
        [](void* o) { static_cast<RingPair*>(o)->work.notify(); }, &p);

    constexpr uint64_t kRecords = 400000;
    constexpr TypeList<uint64_t> kArgs{};
    std::vector<uint8_t> scratch(kInlinePayloadMax);

    uint64_t consumed = 0, bad = 0, bytesSeen = 0;
    std::thread consumer([&] {
        for (;;) {
            const size_t n = p.ring.consume([&](Op op, const uint8_t* rec) {
                if (op != Op::BufferSubData) {
                    ++bad;
                    return;
                }
                cmd::decodeArgs<uint64_t>(rec, [&](uint64_t seq) {
                    const PayloadView v = payloadOf(rec, kArgs);
                    if (seq != consumed || v.blob) ++bad;
                    const auto* d = static_cast<const uint8_t*>(v.data);
                    for (size_t i = 0; i < v.bytes; ++i)
                        if (d[i] != patternByte(seq, i)) {
                            ++bad;
                            break;
                        }
                    bytesSeen += v.bytes;
                });
                ++consumed;
            });
            if (n) {
                p.progress.notify();
                continue;
            }
            if (p.done.load(std::memory_order_acquire) && !p.ring.hasPublished()) break;
            p.work.sleepUnless([&] { return p.ring.hasPublished() || p.done.load(std::memory_order_acquire); });
        }
    });

    uint64_t bytesSent = 0;
    for (uint64_t seq = 0; seq < kRecords; ++seq) {
        // Mostly small records, some a few KiB, now and then the largest
        // inline payload (a quarter of the ring).
        size_t bytes = (seq * 13) % 64;
        if (seq % 97 == 0) bytes = 3000 + (seq % 1000);
        if (seq % 1009 == 0) bytes = kInlinePayloadMax;
        for (size_t i = 0; i < bytes; ++i) scratch[i] = patternByte(seq, i);
        encodePayload(p.ring, Op::BufferSubData, kArgs, scratch.data(), bytes, nullptr, seq);
        bytesSent += bytes;
    }
    p.ring.publish();
    p.done.store(true, std::memory_order_release);
    p.work.notify();
    consumer.join();

    assert(bad == 0);
    assert(consumed == kRecords);
    assert(bytesSeen == bytesSent);
    assert(!p.ring.pending());
    std::cout << "PASSED (" << kRecords << " records, " << (bytesSent >> 20) << " MiB, " << p.fullWaits
              << " full waits)" << std::endl;
}

// --- The WebGL thread with real contexts ------------------------------------

constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kStaticDraw = 0x88E4;

// The drawing buffer's first pixel.
std::vector<uint8_t> firstPixel(WebGL2RenderingContext& ctx) {
    std::vector<uint8_t> px;
    assert(ctx.readCanvasPixels(px));
    return {px[0], px[1], px[2], px[3]};
}

GLenum getError(WebGLClient& cl) {
    return cl.sync([](vk::WebGLVkContext& gl) { return gl.getError(); });
}

// Test 2: a round trip waits for a deep backlog (many ring laps) to replay.
void testSyncBehindBacklog(render::VulkanContext& device, const std::shared_ptr<WebGLThread>& thread) {
    std::cout << "[Test 2] round trip behind a backlog of many ring laps... " << std::flush;
    WebGL2RenderingContext ctx(32, 32, device, {}, thread);
    WebGLClient& cl = *ctx.client();
    constexpr int kCalls = 300000;  // ~ 7 MiB of records: about seven laps
    for (int i = 0; i < kCalls; ++i) {
        cl.viewport(0, 0, 32 - (i & 7), 32);
        cl.clearColor(static_cast<float>(i & 1), 0.0f, 0.0f, 1.0f);
    }
    cl.viewport(0, 0, 32, 32);
    cl.clearColor(0.0f, 0.0f, 1.0f, 1.0f);
    cl.clear(GL_COLOR_BUFFER_BIT);
    assert(firstPixel(ctx) == (std::vector<uint8_t>{0, 0, 255, 255}));
    assert(getError(cl) == GL_NO_ERROR);
    // An error raised by the client lands behind what was encoded before it.
    cl.syntheticError(GL_INVALID_VALUE);
    cl.syntheticError(GL_INVALID_ENUM);  // not the first: dropped
    assert(getError(cl) == GL_INVALID_VALUE);
    assert(getError(cl) == GL_NO_ERROR);
    std::cout << "PASSED" << std::endl;
}

// Test 3: past the blob budget the client waits for the replay to free
// blobs, and every upload lands in order.
void testBlobBudget(render::VulkanContext& device, const std::shared_ptr<WebGLThread>& thread) {
    std::cout << "[Test 3] blob budget back-pressure... " << std::flush;
    WebGL2RenderingContext ctx(16, 16, device, {}, thread);
    WebGLClient& cl = *ctx.client();
    const GLuint buf = ctx.newObjectId();
    cl.createBuffer(buf);
    cl.bindBuffer(kArrayBuffer, WebGLBuffer{buf});
    constexpr size_t kBytes = size_t{3} << 20;  // a blob each
    std::vector<uint8_t> data(kBytes);
    const int kUploads = static_cast<int>((WebGLClient::kBlobCap * 3) / kBytes);  // three budgets' worth
    for (int i = 0; i < kUploads; ++i) {
        std::memset(data.data(), i & 0xff, kBytes);
        data[kBytes - 1] = static_cast<uint8_t>(0x80 | (i & 0x7f));
        cl.bufferData(kArrayBuffer, static_cast<GLsizeiptr>(kBytes), data.data(), kStaticDraw);
        std::memset(data.data(), 0xEE, kBytes);  // the copy was taken at the call
    }
    uint8_t got[2] = {};
    cl.sync([&](vk::WebGLVkContext& gl) {
        gl.getBufferSubData(kArrayBuffer, 0, &got[0], 1);
        gl.getBufferSubData(kArrayBuffer, static_cast<GLintptr>(kBytes - 1), &got[1], 1);
    });
    const int last = kUploads - 1;
    assert(got[0] == static_cast<uint8_t>(last & 0xff));
    assert(got[1] == static_cast<uint8_t>(0x80 | (last & 0x7f)));
    assert(getError(cl) == GL_NO_ERROR);
    std::cout << "PASSED (" << kUploads << " x " << (kBytes >> 20) << " MiB)" << std::endl;
}

// Test 4: many contexts on one thread, their commands interleaved.
void testManyContexts(render::VulkanContext& device, const std::shared_ptr<WebGLThread>& thread) {
    std::cout << "[Test 4] many contexts on one thread... " << std::flush;
    constexpr int kContexts = 8;
    std::vector<std::unique_ptr<WebGL2RenderingContext>> ctxs;
    for (int i = 0; i < kContexts; ++i)
        ctxs.push_back(std::make_unique<WebGL2RenderingContext>(8 + i, 8, device, ContextAttributes{}, thread));
    for (int round = 0; round < 2000; ++round)
        for (int i = 0; i < kContexts; ++i) {
            WebGLClient& cl = *ctxs[static_cast<size_t>(i)]->client();
            cl.clearColor(static_cast<float>(i) / 255.0f, static_cast<float>(round & 0xff) / 255.0f, 0.0f, 1.0f);
            cl.clear(GL_COLOR_BUFFER_BIT);
            if (round % 500 == 0) cl.flush();
        }
    for (int i = 0; i < kContexts; ++i) {
        const auto px = firstPixel(*ctxs[static_cast<size_t>(i)]);
        assert(px[0] == i && px[1] == (1999 & 0xff) && px[3] == 255);
        // The composite's hand-off: an image to sample, its work submitted.
        const render::LayerImage img = ctxs[static_cast<size_t>(i)]->frameEnd();
        assert(img.image != VK_NULL_HANDLE && img.width == static_cast<uint32_t>(8 + i));
    }
    // Destroyed in an order unlike their creation's.
    for (int i = 0; i < kContexts; i += 2) ctxs[static_cast<size_t>(i)].reset();
    ctxs.clear();
    std::cout << "PASSED" << std::endl;
}

// Test 5: a context destroyed with a deep backlog (blobs, held pixels,
// unanswered publications) still pending; the thread goes on serving others.
void testTeardownWithBacklog(render::VulkanContext& device, const std::shared_ptr<WebGLThread>& thread) {
    std::cout << "[Test 5] teardown with a backlog... " << std::flush;
    for (int rep = 0; rep < 20; ++rep) {
        auto ctx = std::make_unique<WebGL2RenderingContext>(64, 64, device, ContextAttributes{}, thread);
        WebGLClient& cl = *ctx->client();
        const GLuint buf = ctx->newObjectId();
        cl.createBuffer(buf);
        cl.bindBuffer(kArrayBuffer, WebGLBuffer{buf});
        std::vector<uint8_t> big(256 * 1024, 7);
        for (int i = 0; i < 40; ++i) {
            cl.bufferData(kArrayBuffer, static_cast<GLsizeiptr>(big.size()), big.data(), kStaticDraw);
            for (int k = 0; k < 2000; ++k) cl.clear(GL_COLOR_BUFFER_BIT);
        }
        const GLuint fence = ctx->newObjectId();
        assert(cl.fenceSync(fence, 0x9117, 0));
        ctx.reset();  // nothing waited for
    }
    WebGL2RenderingContext after(8, 8, device, {}, thread);
    after.client()->clearColor(0.0f, 1.0f, 0.0f, 1.0f);
    after.client()->clear(GL_COLOR_BUFFER_BIT);
    assert(firstPixel(after) == (std::vector<uint8_t>{0, 255, 0, 255}));
    std::cout << "PASSED" << std::endl;
}

// Test 6: context loss and restore with commands pending, and a resize
// between frames.
void testLoseRestoreResize(render::VulkanContext& device, const std::shared_ptr<WebGLThread>& thread) {
    std::cout << "[Test 6] lose / restore / resize with commands pending... " << std::flush;
    WebGL2RenderingContext ctx(16, 16, device, {}, thread);
    for (int rep = 0; rep < 10; ++rep) {
        WebGLClient& cl = *ctx.client();
        for (int k = 0; k < 5000; ++k) {
            cl.clearColor(1.0f, 0.0f, 0.0f, 1.0f);
            cl.clear(GL_COLOR_BUFFER_BIT);
        }
        ctx.loseContext();
        assert(ctx.client() == nullptr && ctx.isContextLost());
        assert(ctx.frameEnd().image == VK_NULL_HANDLE);
        assert(ctx.takeContextEvent() == WebGL2RenderingContext::ContextEvent::Lost);
        ctx.restoreContext();
        assert(ctx.takeContextEvent() == WebGL2RenderingContext::ContextEvent::Restored);
        WebGLClient& again = *ctx.client();
        // A restored context starts from default state: cleared to zero.
        assert(firstPixel(ctx) == (std::vector<uint8_t>{0, 0, 0, 0}));
        again.clearColor(0.0f, 0.0f, 1.0f, 1.0f);
        again.clear(GL_COLOR_BUFFER_BIT);
        ctx.resize(16 + rep, 16);
        again.clear(GL_COLOR_BUFFER_BIT);
        std::vector<uint8_t> px;
        assert(ctx.readCanvasPixels(px));
        assert(px.size() == static_cast<size_t>((16 + rep) * 16 * 4));
        assert(px[2] == 255 && px[3] == 255);
    }
    std::cout << "PASSED" << std::endl;
}

// Test 7: a context made alone makes (and joins) a thread of its own.
void testOwnThread(render::VulkanContext& device) {
    std::cout << "[Test 7] a context's own thread... " << std::flush;
    for (int rep = 0; rep < 5; ++rep) {
        WebGL2RenderingContext ctx(8, 8, device);
        ctx.client()->clearColor(1.0f, 1.0f, 0.0f, 1.0f);
        ctx.client()->clear(GL_COLOR_BUFFER_BIT);
        if (rep & 1) assert(firstPixel(ctx) == (std::vector<uint8_t>{255, 255, 0, 255}));
    }
    std::cout << "PASSED" << std::endl;
}

}  // namespace

int main() {
    std::cout << "=== bro_webgl_ring_test: the WebGL command ring and thread ===" << std::endl;
    testRingStress();

    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;
    render::VulkanContext device(cfg);
    if (!device.init()) {
        std::cerr << "FAILED: Could not initialize VulkanContext" << std::endl;
        return 1;
    }
    {
        auto thread = std::make_shared<WebGLThread>();
        testSyncBehindBacklog(device, thread);
        testBlobBudget(device, thread);
        testManyContexts(device, thread);
        testTeardownWithBacklog(device, thread);
        testLoseRestoreResize(device, thread);
        std::cout << "(the WebGL thread was woken " << thread->wakes() << " times)" << std::endl;
    }
    testOwnThread(device);
    device.queue().waitIdle();
    std::cout << "=== all passed ===" << std::endl;
    return 0;
}

#pragma once

// WebGLClient: the JS thread's side of one WebGL2 context. The binding calls
// it instead of the backend (vk::WebGLVkContext): fire-and-forget calls are
// encoded into the context's ring (webgl_cmd_list.h) with their data copied
// at call time, and calls that answer something are sync() round trips that
// run against the backend once every command before them has replayed.
//
// THE ORDERING RULE: every backend call happens in API order. Encoded calls
// replay in ring order; a sync() or drained() replays the ring first; so does
// every call the binding still makes on the backend directly
// (WebGL2RenderingContext::backend() drains). Errors the binding raises
// itself go down the ring (syntheticError), behind the commands before them,
// which keeps getError exact.
//
// THE DATA RULE: the backend never sees memory the JS heap owns outside a
// sync(). Encoded calls copy their typed-array data into the record (or a
// blob, past kInlinePayloadMax), so the array may change or be collected
// before the replay; only a sync() callback, which runs while the caller
// waits, may use a pointer into the heap.
//
// Inline replay (for now): the ring is replayed on the calling thread at
// sync points (sync, drained, flush, the frame's end) and when it is full.
// The consumer becomes the WebGL thread later; the encoding stays.

#include "webgl/client/webgl_cmd.h"
#include "webgl/client/webgl_encoder.h"
#include "webgl/client/webgl_ring.h"

#include <atomic>
#include <memory>
#include <utility>

namespace bro::webgl {

namespace vk { class WebGLVkContext; }

class WebGLClient {
public:
    /// Blob bytes outstanding at most before the client waits for replays to
    /// free some (one larger payload is still taken, alone).
    static constexpr size_t kBlobCap = size_t{64} << 20;

    WebGLClient();
    /// Pending commands are dropped (their blobs freed), then the backend
    /// goes.
    ~WebGLClient();

    WebGLClient(const WebGLClient&) = delete;
    WebGLClient& operator=(const WebGLClient&) = delete;

    // --- The backend (inline replay owns it) ---
    void attach(std::unique_ptr<vk::WebGLVkContext> backend);
    /// Replay what is pending, then destroy the backend (context loss).
    void detach();
    bool hasBackend() const { return backend_ != nullptr; }

    /// Replay every pending command onto the backend.
    void drain() {
        if (ring_.pending()) replayPending();
    }
    /// The backend with every pending command replayed: for calls not yet
    /// encoded. Null without one.
    vk::WebGLVkContext* drained() {
        drain();
        return backend_.get();
    }
    /// A round trip: fn(backend) after every pending command, its answer
    /// returned. Requires a backend.
    template <class F>
    decltype(auto) sync(F&& fn) {
        drain();
        return std::forward<F>(fn)(*backend_);
    }

    // --- Fire-and-forget: one method per BRO_WEBGL_SIMPLE_COMMANDS line,
    //     named and ordered like the backend's (object arguments typed, e.g.
    //     WebGLBuffer{id}) ---
#define BRO_WEBGL_CLIENT_SIMPLE(Name, method, ...)                                 \
    template <class... U>                                                          \
    [[gnu::always_inline]] void method(U... u) {                                   \
        encodeSimple(ring_, Op::Name, TypeList<__VA_ARGS__>{}, u...);              \
    }
    BRO_WEBGL_SIMPLE_COMMANDS(BRO_WEBGL_CLIENT_SIMPLE)
#undef BRO_WEBGL_CLIENT_SIMPLE

    // --- Payload calls: the backend's arguments, then the data and its
    //     element count (what the backend reads), copied now ---
#define BRO_WEBGL_CLIENT_P1(Name, method, Elem, A1)                                \
    void method(A1 a1, const Elem* v, size_t n) {                                  \
        putPayload(Op::Name, TypeList<A1>{}, v, n * sizeof(Elem), a1);             \
    }
#define BRO_WEBGL_CLIENT_P2(Name, method, Elem, A1, A2)                            \
    void method(A1 a1, A2 a2, const Elem* v, size_t n) {                           \
        putPayload(Op::Name, TypeList<A1, A2>{}, v, n * sizeof(Elem), a1, a2);     \
    }
#define BRO_WEBGL_CLIENT_P3(Name, method, Elem, A1, A2, A3)                        \
    void method(A1 a1, A2 a2, A3 a3, const Elem* v, size_t n) {                    \
        putPayload(Op::Name, TypeList<A1, A2, A3>{}, v, n * sizeof(Elem), a1, a2, a3); \
    }
    BRO_WEBGL_PAYLOAD_COMMANDS(BRO_WEBGL_CLIENT_P1, BRO_WEBGL_CLIENT_P2, BRO_WEBGL_CLIENT_P3)
#undef BRO_WEBGL_CLIENT_P1
#undef BRO_WEBGL_CLIENT_P2
#undef BRO_WEBGL_CLIENT_P3

    /// bufferData with `size` bytes of `data`, or allocation only (data null).
    void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
        const uint64_t bytes = data ? static_cast<uint64_t>(size) : kNullPayload;
        putPayload(Op::BufferData, kBufferDataArgs, data, bytes, target, usage, size);
    }
    void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
        putPayload(Op::BufferSubData, kBufferSubDataArgs, data, static_cast<uint64_t>(size), target, offset);
    }

    // --- The client's own records ---
    /// An error the binding raised, in order with the backend's.
    void syntheticError(GLenum error) { encodeSimple(ring_, Op::SyntheticError, TypeList<GLenum>{}, error); }
    /// Submit what is recorded (WebGL's flush, the frame's end): a sync point.
    void flush();
    /// Submit and wait for the GPU: a round trip.
    void finish();
    /// The engine has shown the drawing buffer.
    void presented() { encodeSimple(ring_, Op::Presented, TypeList<>{}); }
    /// Resize the drawing buffer; `presentedTicket` is the last queue
    /// submission that may sample the old one.
    void resize(int width, int height, uint64_t presentedTicket) {
        encodeSimple(ring_, Op::Resize, TypeList<int, int, uint64_t>{}, width, height, presentedTicket);
    }

    // The hand-written records' argument lists (webgl_replay.cpp decodes them).
    static constexpr TypeList<GLenum, GLenum, GLsizeiptr> kBufferDataArgs{};
    static constexpr TypeList<GLenum, GLintptr> kBufferSubDataArgs{};

private:
    template <class... Ts, class... Us>
    void putPayload(Op op, TypeList<Ts...> types, const void* data, uint64_t bytes, Us... u) {
        void* blob = nullptr;
        if (bytes != kNullPayload && bytes > kInlinePayloadMax) blob = copyToBlob(data, bytes);
        encodePayload(ring_, op, types, data, bytes, blob, u...);
    }
    /// A malloc'd copy of a large payload, counted against kBlobCap (replays
    /// first when over it).
    void* copyToBlob(const void* data, uint64_t bytes);
    void replayPending();
    static void onRingFull(void* self);

    WebGLRing ring_;
    std::unique_ptr<vk::WebGLVkContext> backend_;
    std::atomic<size_t> blobBytes_{0};
    bool replaying_ = false;
};

/// Replay one record onto `gl`, or only release what it owns (a blob) when
/// `gl` is null (webgl_replay.cpp). `blobBytes` is decremented by a blob's
/// size when it is freed.
void replayWebGLCommand(vk::WebGLVkContext* gl, Op op, const uint8_t* rec, std::atomic<size_t>& blobBytes);

}  // namespace bro::webgl

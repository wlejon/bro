#pragma once

// WebGLClient: the JS thread's side of one WebGL2 context. The binding calls
// it instead of the backend (vk::WebGLVkContext): fire-and-forget calls are
// encoded into the context's ring (webgl_cmd_list.h) with their data copied
// at call time, and calls that answer something are sync() round trips that
// run against the backend once every command before them has replayed.
//
// THE ORDERING RULE: every backend call happens in API order. Encoded calls
// replay in ring order, and a sync() replays the ring first. Errors the
// binding or the client raises itself go down the ring (syntheticError),
// behind the commands before them, which keeps getError exact.
//
// THE ANSWERS: what the page reads often is answered here without asking
// the backend (webgl_client_async.h): object names, the device's caps, the
// extensions enabled, each program's last link and each shader's last
// compile (published by the replay; the first read after a link or compile
// waits for that one publication), each query's status and result and each
// fence's ticket (published by the replay, read without waiting).
//
// THE DATA RULE: the backend never sees memory the JS heap owns outside a
// sync(). Encoded calls copy their typed-array data into the record (or a
// blob, past kInlinePayloadMax), so the array may change or be collected
// before the replay; only a sync() callback, which runs while the caller
// waits, may use a pointer into the heap.
//
// Inline replay (for now): the ring is replayed on the calling thread at
// sync points (sync, flush, the frame's end, a read of a publication not
// made yet) and when it is full. The consumer becomes the WebGL thread
// later; the encoding and the publications stay.

#include "webgl/client/webgl_client_async.h"
#include "webgl/client/webgl_client_records.h"
#include "webgl/client/webgl_cmd.h"
#include "webgl/client/webgl_encoder.h"
#include "webgl/client/webgl_ring.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::render { class VulkanQueue; }

namespace bro::webgl {

namespace vk {
class WebGLVkContext;
struct WebGLCaps;
}  // namespace vk

class WebGLClient {
public:
    /// Blob bytes outstanding at most before the client waits for replays to
    /// free some (one larger payload is still taken, alone).
    static constexpr size_t kBlobCap = size_t{64} << 20;

    /// `caps` and `queue` outlive the client (WebGL2RenderingContext's).
    WebGLClient(const vk::WebGLCaps& caps, render::VulkanQueue& queue);
    /// Pending commands are dropped (their blobs freed), then the backend
    /// goes.
    ~WebGLClient();

    WebGLClient(const WebGLClient&) = delete;
    WebGLClient& operator=(const WebGLClient&) = delete;

    // --- The backend (inline replay owns it) ---
    void attach(std::unique_ptr<vk::WebGLVkContext> backend);
    /// Replay what is pending, then destroy the backend (context loss), and
    /// forget every object and extension with it.
    void detach();
    bool hasBackend() const { return backend_ != nullptr; }

    /// Replay every pending command onto the backend.
    void drain() {
        if (ring_.pending()) replayPending();
    }
    /// A round trip: fn(backend) after every pending command, its answer
    /// returned. Requires a backend. `fn` is plain C++ that never touches
    /// the JS heap but for a pointer the caller took just before (the JS
    /// thread waits while it runs, so nothing moves).
    template <class F>
    auto sync(F&& fn) {
        drain();
        if constexpr (std::is_void_v<std::invoke_result_t<F, vk::WebGLVkContext&>>) {
            std::forward<F>(fn)(*backend_);
            replay_.publish(*backend_);
        } else {
            auto r = std::forward<F>(fn)(*backend_);
            replay_.publish(*backend_);
            return r;
        }
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

#define BRO_WEBGL_CLIENT_BYTES(Name, method, ...)                                  \
    template <class... U>                                                          \
    void method(U... u) {                                                          \
        putBytes(Op::Name, TypeList<__VA_ARGS__>{}, u...);                         \
    }
    BRO_WEBGL_BYTES_COMMANDS(BRO_WEBGL_CLIENT_BYTES)
#undef BRO_WEBGL_CLIENT_BYTES

    // --- String calls: the backend's arguments, then the string ---
#define BRO_WEBGL_CLIENT_STRING(Name, method, ...)                                 \
    template <class... U>                                                          \
    void method(U... u) {                                                          \
        putString(Op::Name, TypeList<__VA_ARGS__>{}, u...);                        \
    }
    BRO_WEBGL_STRING_COMMANDS(BRO_WEBGL_CLIENT_STRING)
#undef BRO_WEBGL_CLIENT_STRING

    /// bufferData with `size` bytes of `data`, or allocation only (data null).
    void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
        const uint64_t bytes = data ? static_cast<uint64_t>(size) : kNullPayload;
        putPayload(Op::BufferData, kBufferDataArgs, data, bytes, target, usage, size);
    }
    void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
        putPayload(Op::BufferSubData, kBufferSubDataArgs, data, static_cast<uint64_t>(size), target, offset);
    }

    // --- DOM-source uploads (webgl_client_textures.cpp) ---
    /// The pixels of a source that may change before the replay (a canvas
    /// snapshot, a video frame, ImageData), copied and counted.
    rec::PixelRef copiedPixels(const uint8_t* rgba, uint32_t width, uint32_t height);
    /// The pixels of a decoded image, held, not copied.
    static rec::PixelRef heldPixels(std::shared_ptr<const uint8_t> rgba, uint32_t width, uint32_t height);
    void texImage2DSource(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, rec::PixelRef pixels);
    void texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                             GLsizei height, GLenum format, GLenum type, rec::PixelRef pixels);
    void invalidateFramebuffer(GLenum target, const GLenum* attachments, size_t n);
    void invalidateSubFramebuffer(GLenum target, const GLenum* attachments, size_t n, GLint x, GLint y,
                                  GLsizei width, GLsizei height);
    void transformFeedbackVaryings(GLuint program, const std::vector<std::string>& varyings, GLenum bufferMode);

    // --- Shaders and programs (webgl_client_programs.cpp) ---
    /// createShader: false (INVALID_ENUM raised, nothing created) for a type
    /// that is neither VERTEX_SHADER nor FRAGMENT_SHADER.
    bool newShader(GLuint id, GLenum type);
    void dropShader(WebGLShader s);
    void setShaderSource(WebGLShader s, std::string_view source);
    void compileShader(WebGLShader s);
    void newProgram(GLuint id);
    void dropProgram(WebGLProgram p);
    void linkProgram(WebGLProgram p);
    /// The last link of program `id`, once published (the first read after a
    /// link waits for it). Null when the client does not hold the program
    /// (never made, deleted): the caller asks the backend instead.
    const ProgramLink* programLink(GLuint id);
    /// The last compile of shader `id`, likewise.
    const ShaderCompile* shaderCompile(GLuint id);
    /// What shaderSource last set on shader `id`; null when not held.
    const std::string* heldShaderSource(GLuint id) const;
    /// COMPLETION_STATUS_KHR, never waiting: the last link (compile) is
    /// published. False for an object the client does not hold.
    bool programSettled(GLuint id) const;
    bool holdsProgram(GLuint id) const { return id != 0 && programs_.count(id) != 0; }
    bool shaderSettled(GLuint id) const;

    // --- Queries (webgl_client_async.cpp) ---
    void createQuery(GLuint id);
    void deleteQuery(GLuint id);
    void beginQuery(GLenum target, GLuint id);
    void endQuery(GLenum target);
    void queryCounter(GLuint id, GLenum target);
    /// The query's status after every call on it replayed (waiting once
    /// when one has not), and its result when Available. False when the
    /// client does not hold the query (never made, deleted).
    bool queryStatus(GLuint id, QueryStatus& status, uint64_t& result);

    // --- Syncs, answered here entirely (webgl_client_async.cpp) ---
    /// fenceSync: false (the error raised, nothing made) for a condition
    /// or flags WebGL refuses.
    bool fenceSync(GLuint id, GLenum condition, GLbitfield flags);
    void dropFence(GLuint id);
    bool isFence(GLuint id) const { return id != 0 && fences_.count(id) != 0; }
    GLenum clientWaitSync(GLuint id, GLbitfield flags, double timeoutNs);
    void waitSync(GLuint id, GLbitfield flags, double timeoutNs);
    /// False (the error raised) for a deleted sync or an unknown pname.
    bool getSyncParameter(GLuint id, GLenum pname, GLint& out);

    // --- Extensions (webgl_client_async.cpp) ---
    /// getExtension: whether the device has it; enabling it on the backend
    /// is a command, its state mirrored here.
    bool getExtension(const std::string& name);
    bool extensionEnabled(std::string_view name) const;
    /// COMPRESSED_TEXTURE_FORMATS: the formats of the compressed-texture
    /// extensions enabled, in the device's order.
    std::vector<GLint> compressedTextureFormats() const;
    const vk::WebGLCaps& caps() const { return caps_; }

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
    /// Split the trailing (data, size) off and encode the rest as a payload.
    template <class... Ts, class... Us>
    void putBytes(Op op, TypeList<Ts...> types, Us... u) {
        static_assert(sizeof...(Us) == sizeof...(Ts) + 2, "argument count differs from the command table");
        auto args = std::make_tuple(u...);
        const void* data = std::get<sizeof...(Ts)>(args);
        const uint64_t bytes = data ? static_cast<uint64_t>(std::get<sizeof...(Ts) + 1>(args)) : kNullPayload;
        [&]<size_t... I>(std::index_sequence<I...>) {
            putPayload(op, types, data, bytes, std::get<I>(args)...);
        }(std::index_sequence_for<Ts...>{});
    }
    /// Split the trailing string off and encode the rest with its bytes.
    template <class... Ts, class... Us>
    void putString(Op op, TypeList<Ts...> types, Us... u) {
        static_assert(sizeof...(Us) == sizeof...(Ts) + 1, "argument count differs from the command table");
        auto args = std::make_tuple(u...);
        const std::string_view str(std::get<sizeof...(Ts)>(args));
        [&]<size_t... I>(std::index_sequence<I...>) {
            putPayload(op, types, str.data(), str.size(), std::get<I>(args)...);
        }(std::index_sequence_for<Ts...>{});
    }
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
    /// Forget every object and extension (the backend went).
    void forgetObjects();
    /// Over the blob cap: wait for the replay to free some.
    void reserveBlobBytes(uint64_t bytes);

    WebGLRing ring_;
    std::unique_ptr<vk::WebGLVkContext> backend_;
    WebGLReplayState replay_;  // the replay's (the consumer's) state
    bool replaying_ = false;
    const vk::WebGLCaps& caps_;
    render::VulkanQueue& queue_;

    // The JS thread's own tables (never read by the replay).
    struct HeldProgram {
        std::shared_ptr<ProgramSlot> slot;
        uint32_t links = 0;  // linkProgram calls encoded
    };
    struct HeldShader {
        std::shared_ptr<ShaderSlot> slot;
        uint32_t compiles = 0;
        std::string source;
    };
    struct HeldQuery {
        std::shared_ptr<QuerySlot> slot;
        uint32_t serial = 0;  // begin/end/queryCounter calls encoded on it
    };
    std::unordered_map<GLuint, HeldProgram> programs_;
    std::unordered_map<GLuint, HeldShader> shaders_;
    std::unordered_map<GLuint, HeldQuery> queries_;
    std::unordered_map<GLuint, std::shared_ptr<FenceSlot>> fences_;
    /// The query the client last began on each target group (occlusion,
    /// primitives written, time elapsed): the one an endQuery names.
    GLuint activeQuery_[3]{};
    std::vector<std::string> extensions_;  // enabled, in order
};

/// Replay one record onto `gl`, or only release what it owns (a blob, a
/// held reference) when `gl` is null (webgl_replay.cpp).
void replayWebGLCommand(vk::WebGLVkContext* gl, Op op, const uint8_t* rec, WebGLReplayState& state);

}  // namespace bro::webgl

// The replay: one record of the ring onto the backend, decoded by the same
// command table the client encodes with (webgl_cmd_list.h).

#include "webgl/client/webgl_client.h"

#include "webgl/vulkan/webgl_vk_context.h"

#include <cstdlib>

namespace bro::webgl {

namespace {

// Call f(payload pointer, args...) for a payload record, then free its blob.
template <class... Ts, class F>
void replayPayload(vk::WebGLVkContext* gl, const uint8_t* rec, std::atomic<size_t>& blobBytes, TypeList<Ts...> types,
                   F&& f) {
    const PayloadView p = payloadOf(rec, types);
    if (gl) cmd::decodeArgs<Ts...>(rec, [&](auto... a) { f(p, a...); });
    if (p.blob) {
        std::free(const_cast<void*>(p.data));
        blobBytes.fetch_sub(static_cast<size_t>(p.bytes), std::memory_order_release);
    }
}

}  // namespace

void replayWebGLCommand(vk::WebGLVkContext* gl, Op op, const uint8_t* rec, std::atomic<size_t>& blobBytes) {
    switch (op) {
#define BRO_WEBGL_REPLAY_SIMPLE(Name, method, ...)                                 \
    case Op::Name:                                                                 \
        if (gl) cmd::decodeArgs<__VA_ARGS__>(rec, [gl](auto... a) { gl->method(a...); }); \
        return;
        BRO_WEBGL_SIMPLE_COMMANDS(BRO_WEBGL_REPLAY_SIMPLE)
        BRO_WEBGL_CONTROL_COMMANDS(BRO_WEBGL_REPLAY_SIMPLE)
#undef BRO_WEBGL_REPLAY_SIMPLE

#define BRO_WEBGL_REPLAY_PAYLOAD(Name, method, Elem, ...)                          \
    case Op::Name:                                                                 \
        replayPayload(gl, rec, blobBytes, TypeList<__VA_ARGS__>{}, [gl](const PayloadView& p, auto... a) { \
            gl->method(a..., static_cast<const Elem*>(p.data));                   \
        });                                                                        \
        return;
        BRO_WEBGL_PAYLOAD_COMMANDS(BRO_WEBGL_REPLAY_PAYLOAD, BRO_WEBGL_REPLAY_PAYLOAD, BRO_WEBGL_REPLAY_PAYLOAD)
#undef BRO_WEBGL_REPLAY_PAYLOAD

        case Op::BufferData:
            replayPayload(gl, rec, blobBytes, WebGLClient::kBufferDataArgs,
                          [gl](const PayloadView& p, GLenum target, GLenum usage, GLsizeiptr size) {
                              gl->bufferData(target, size, p.data, usage);
                          });
            return;
        case Op::BufferSubData:
            replayPayload(gl, rec, blobBytes, WebGLClient::kBufferSubDataArgs,
                          [gl](const PayloadView& p, GLenum target, GLintptr offset) {
                              gl->bufferSubData(target, offset, static_cast<GLsizeiptr>(p.bytes), p.data);
                          });
            return;

        case Op::Wrap:
        case Op::Count:
            break;
    }
    std::abort();  // a record the table does not know: the ring is corrupt
}

}  // namespace bro::webgl

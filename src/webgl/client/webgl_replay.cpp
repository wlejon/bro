// The replay: one record of the ring onto the backend, decoded by the same
// command table the client encodes with (webgl_cmd_list.h), and the
// hand-written records (webgl_client_records.h).

#include "webgl/client/webgl_client.h"

#include "webgl/vulkan/webgl_vk_context.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace bro::webgl {

namespace {

// Call f(payload, args...) for a payload record, then give its blob back.
template <class... Ts, class F>
void replayPayload(vk::WebGLVkContext* gl, const uint8_t* rec, WebGLReplayState& state, TypeList<Ts...> types,
                   F&& f) {
    const PayloadView p = payloadOf(rec, types);
    if (gl) cmd::decodeArgs<Ts...>(rec, [&](auto... a) { f(p, a...); });
    if (p.blob) {
        state.blobs.give(p.blob);
        state.blobBytes.fetch_sub(static_cast<size_t>(p.bytes), std::memory_order_release);
    }
}

std::string payloadString(const PayloadView& p) {
    return std::string(static_cast<const char*>(p.data), static_cast<size_t>(p.bytes));
}

// A DOM-source upload: the pixels it holds go once replayed.
template <class... Ts, class F>
void replaySource(vk::WebGLVkContext* gl, const uint8_t* rec, WebGLReplayState& state, TypeList<Ts...>, F&& f) {
    cmd::decodeArgs<Ts...>(rec, [&](auto... a) {
        void* held = std::get<sizeof...(Ts) - 1>(std::make_tuple(a...));
        std::unique_ptr<rec::PixelRef> ref(static_cast<rec::PixelRef*>(held));
        if (gl) f(*ref, a...);
        if (ref->counted) state.blobBytes.fetch_sub(ref->counted, std::memory_order_release);
    });
}

void replayCustom(vk::WebGLVkContext* gl, Op op, const uint8_t* rec, WebGLReplayState& state) {
    switch (op) {
        case Op::BufferData:
            replayPayload(gl, rec, state, WebGLClient::kBufferDataArgs,
                          [gl](const PayloadView& p, GLenum target, GLenum usage, GLsizeiptr size) {
                              if (p.blob)  // the backend may keep the blob's bytes as the buffer's
                                  gl->bufferDataTake(target, p.blob->bytes, usage);
                              else
                                  gl->bufferData(target, size, p.data, usage);
                          });
            return;
        case Op::BufferSubData:
            replayPayload(gl, rec, state, WebGLClient::kBufferSubDataArgs,
                          [gl](const PayloadView& p, GLenum target, GLintptr offset) {
                              if (p.blob)
                                  gl->bufferSubDataTake(target, offset, p.blob->bytes);
                              else
                                  gl->bufferSubData(target, offset, static_cast<GLsizeiptr>(p.bytes), p.data);
                          });
            return;
        case Op::TexImage2DSource:
            replaySource(gl, rec, state, rec::kTexImage2DSource,
                         [gl](const rec::PixelRef& px, GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLenum format, GLenum type, void*) {
                             gl->texImage2DSource(target, level, internalformat, width, height, format, type,
                                                  px.rgba.get(), px.width, px.height);
                         });
            return;
        case Op::TexSubImage2DSource:
            replaySource(gl, rec, state, rec::kTexSubImage2DSource,
                         [gl](const rec::PixelRef& px, GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                              GLsizei height, GLenum format, GLenum type, void*) {
                             gl->texSubImage2DSource(target, level, x, y, width, height, format, type,
                                                     px.rgba.get(), px.width, px.height);
                         });
            return;
        case Op::InvalidateFramebuffer:
            replayPayload(gl, rec, state, rec::kInvalidateFramebuffer, [gl](const PayloadView& p, GLenum target) {
                gl->invalidateFramebuffer(
                    target, std::span<const GLenum>(static_cast<const GLenum*>(p.data), p.bytes / sizeof(GLenum)));
            });
            return;
        case Op::InvalidateSubFramebuffer:
            replayPayload(gl, rec, state, rec::kInvalidateSubFramebuffer,
                          [gl](const PayloadView& p, GLenum target, GLint x, GLint y, GLsizei w, GLsizei h) {
                              gl->invalidateSubFramebuffer(
                                  target,
                                  std::span<const GLenum>(static_cast<const GLenum*>(p.data), p.bytes / sizeof(GLenum)),
                                  x, y, w, h);
                          });
            return;
        case Op::TransformFeedbackVaryings:
            replayPayload(gl, rec, state, rec::kTransformFeedbackVaryings,
                          [gl](const PayloadView& p, GLuint program, GLenum mode, uint32_t count) {
                              std::vector<std::string> names;
                              names.reserve(count);
                              const char* at = static_cast<const char*>(p.data);
                              for (uint32_t i = 0; i < count; ++i) {
                                  names.emplace_back(at);
                                  at += names.back().size() + 1;
                              }
                              gl->transformFeedbackVaryings(program, names, mode);
                          });
            return;
        case Op::CompileShader:
            cmd::decodeArgs<WebGLShader, void*>(rec, [gl](WebGLShader s, void* held) {
                std::shared_ptr<ShaderSlot> slot = takeRef<ShaderSlot>(held);
                if (gl) {
                    replayCompileShader(*gl, s, slot.get());
                } else if (slot) {
                    // No backend (one that could not be made): a failed compile,
                    // published, so no client waits for it.
                    slot->compile = std::make_shared<const ShaderCompile>();
                    slot->compiles.fetch_add(1, std::memory_order_release);
                }
            });
            return;
        case Op::LinkProgram:
            cmd::decodeArgs<WebGLProgram, void*>(rec, [gl](WebGLProgram p, void* held) {
                std::shared_ptr<ProgramSlot> slot = takeRef<ProgramSlot>(held);
                if (gl) {
                    replayLinkProgram(*gl, p, slot.get());
                } else if (slot) {
                    slot->link = emptyProgramLink();
                    slot->links.fetch_add(1, std::memory_order_release);
                }
            });
            return;
        case Op::CreateQuery:
            cmd::decodeArgs<GLuint, void*>(rec, [gl, &state](GLuint id, void* held) {
                std::shared_ptr<QuerySlot> slot = takeRef<QuerySlot>(held);
                if (!gl) return;
                gl->createQuery(id);
                state.createQuery(id, std::move(slot));
            });
            return;
        case Op::DeleteQuery:
            cmd::decodeArgs<GLuint>(rec, [gl, &state](GLuint id) {
                if (!gl) return;
                gl->deleteQuery(id);
                state.deleteQuery(id);
            });
            return;
        case Op::BeginQuery:
            cmd::decodeArgs<GLenum, GLuint>(rec, [gl, &state](GLenum target, GLuint id) {
                if (!gl) return;
                gl->beginQuery(target, id);
                state.queryTouched(id);
            });
            return;
        case Op::EndQuery:
            cmd::decodeArgs<GLenum, GLuint>(rec, [gl, &state](GLenum target, GLuint id) {
                if (!gl) return;
                gl->endQuery(target);
                state.queryTouched(id);
            });
            return;
        case Op::QueryCounter:
            cmd::decodeArgs<GLuint, GLenum>(rec, [gl, &state](GLuint id, GLenum target) {
                if (!gl) return;
                gl->queryCounter(id, target);
                state.queryTouched(id);
            });
            return;
        case Op::FenceSync:
            cmd::decodeArgs<GLuint, GLenum, GLbitfield, void*>(
                rec, [gl](GLuint id, GLenum condition, GLbitfield flags, void* held) {
                    std::shared_ptr<FenceSlot> slot = takeRef<FenceSlot>(held);
                    if (!gl) {
                        if (slot) slot->ticketPlus1.store(1, std::memory_order_release);  // ticket 0: complete
                        return;
                    }
                    gl->fenceSync(id, condition, flags);
                    slot->ticketPlus1.store(gl->syncTicket(id) + 1, std::memory_order_release);
                });
            return;
        default:
            break;
    }
    std::abort();  // a record the table does not know: the ring is corrupt
}

}  // namespace

void replayWebGLCommand(vk::WebGLVkContext* gl, Op op, const uint8_t* rec, WebGLReplayState& state) {
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
        replayPayload(gl, rec, state, TypeList<__VA_ARGS__>{}, [gl](const PayloadView& p, auto... a) { \
            gl->method(a..., static_cast<const Elem*>(p.data));                   \
        });                                                                        \
        return;
        BRO_WEBGL_PAYLOAD_COMMANDS(BRO_WEBGL_REPLAY_PAYLOAD, BRO_WEBGL_REPLAY_PAYLOAD, BRO_WEBGL_REPLAY_PAYLOAD)
#undef BRO_WEBGL_REPLAY_PAYLOAD

#define BRO_WEBGL_REPLAY_BYTES(Name, method, ...)                                  \
    case Op::Name:                                                                 \
        replayPayload(gl, rec, state, TypeList<__VA_ARGS__>{}, [gl](const PayloadView& p, auto... a) { \
            gl->method(a..., p.data, static_cast<size_t>(p.bytes));               \
        });                                                                        \
        return;
        BRO_WEBGL_BYTES_COMMANDS(BRO_WEBGL_REPLAY_BYTES)
#undef BRO_WEBGL_REPLAY_BYTES

#define BRO_WEBGL_REPLAY_STRING(Name, method, ...)                                 \
    case Op::Name:                                                                 \
        replayPayload(gl, rec, state, TypeList<__VA_ARGS__>{}, [gl](const PayloadView& p, auto... a) { \
            gl->method(a..., payloadString(p));                                    \
        });                                                                        \
        return;
        BRO_WEBGL_STRING_COMMANDS(BRO_WEBGL_REPLAY_STRING)
#undef BRO_WEBGL_REPLAY_STRING

        case Op::Wrap:
        case Op::Count:
            break;
        default:
            replayCustom(gl, op, rec, state);
            return;
    }
    std::abort();  // a record the table does not know: the ring is corrupt
}

}  // namespace bro::webgl

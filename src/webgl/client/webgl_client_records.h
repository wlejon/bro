#pragma once

// The argument lists of the hand-written records (BRO_WEBGL_CUSTOM_COMMANDS):
// WebGLClient encodes them, webgl_replay.cpp decodes them, both from these.

#include "webgl/client/webgl_cmd.h"

#include <cstdint>
#include <memory>

namespace bro::webgl::vk { class WebGLVkContext; }

namespace bro::webgl::rec {

/// The pixels of a DOM texture source (img, ImageBitmap, canvas, video,
/// ImageData): tightly packed RGBA8, top row first. A decoded image is held
/// by reference (it never changes); anything else is the client's copy,
/// counted against the blob cap as `counted` bytes.
struct PixelRef {
    std::shared_ptr<const uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t counted = 0;
};

// {target, level, internalformat, width, height, format, type, PixelRef*}
inline constexpr TypeList<GLenum, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*> kTexImage2DSource{};
// {target, level, xoffset, yoffset, width, height, format, type, PixelRef*}
inline constexpr TypeList<GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*> kTexSubImage2DSource{};
// {target} + GLenum attachments
inline constexpr TypeList<GLenum> kInvalidateFramebuffer{};
// {target, x, y, width, height} + GLenum attachments
inline constexpr TypeList<GLenum, GLint, GLint, GLsizei, GLsizei> kInvalidateSubFramebuffer{};
// {program, bufferMode, count} + the names, each ended by a NUL
inline constexpr TypeList<GLuint, GLenum, uint32_t> kTransformFeedbackVaryings{};
// {shader, ShaderSlot ref}
inline constexpr TypeList<WebGLShader, void*> kCompileShader{};
// {program, ProgramSlot ref}
inline constexpr TypeList<WebGLProgram, void*> kLinkProgram{};
// {query, QuerySlot ref}
inline constexpr TypeList<GLuint, void*> kCreateQuery{};
// {query}
inline constexpr TypeList<GLuint> kDeleteQuery{};
// {target, query}
inline constexpr TypeList<GLenum, GLuint> kBeginQuery{};
// {target, the query the client thinks it ends (its serial moves)}
inline constexpr TypeList<GLenum, GLuint> kEndQuery{};
// {query, target}
inline constexpr TypeList<GLuint, GLenum> kQueryCounter{};
// {width, height}
inline constexpr TypeList<int, int> kAttach{};
// {WebGLSyncFn, its context, the round trip's sequence number}
inline constexpr TypeList<void (*)(void*, vk::WebGLVkContext*), void*, uint64_t> kSyncCall{};
// {the frame end's sequence number}
inline constexpr TypeList<uint64_t> kFrameEnd{};
// {sync, condition, flags, FenceSlot ref}
inline constexpr TypeList<GLuint, GLenum, GLbitfield, void*> kFenceSync{};

}  // namespace bro::webgl::rec

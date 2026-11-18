// WebGL 1: what a getContext('webgl') context adds to and takes from the
// WebGL 2 binding it shares. WebGLRenderingContext's prototype is built by
// the same family installers as WebGL2RenderingContext's, less the names
// WebGL 2 added (kWebGL2Only, Chromium's two prototypes diffed), and its
// extensions put the WebGL 2 functionality they stand for under their WebGL 1
// names: ANGLE_instanced_arrays, OES_vertex_array_object and
// WEBGL_draw_buffers call the same client entry points as drawArraysInstanced,
// bindVertexArray and drawBuffers. The texture formats a WebGL 1 program
// names (HALF_FLOAT_OES, SRGB_EXT, SRGB_ALPHA_EXT) are mapped to WebGL 2's
// before the call is encoded (webgl1TexFormat).

#include "bronze_host/webgl_internal.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace bro::bronze_host {

namespace {

// Byte-ordered, for the binary search.
constexpr std::string_view kWebGL2Only[] = {
    "ACTIVE_UNIFORM_BLOCKS", "ALREADY_SIGNALED", "ANY_SAMPLES_PASSED", "ANY_SAMPLES_PASSED_CONSERVATIVE",
    "COLOR", "COLOR_ATTACHMENT1", "COLOR_ATTACHMENT10", "COLOR_ATTACHMENT11", "COLOR_ATTACHMENT12",
    "COLOR_ATTACHMENT13", "COLOR_ATTACHMENT14", "COLOR_ATTACHMENT15", "COLOR_ATTACHMENT2",
    "COLOR_ATTACHMENT3", "COLOR_ATTACHMENT4", "COLOR_ATTACHMENT5", "COLOR_ATTACHMENT6", "COLOR_ATTACHMENT7",
    "COLOR_ATTACHMENT8", "COLOR_ATTACHMENT9", "COMPARE_REF_TO_TEXTURE", "CONDITION_SATISFIED",
    "COPY_READ_BUFFER", "COPY_READ_BUFFER_BINDING", "COPY_WRITE_BUFFER", "COPY_WRITE_BUFFER_BINDING",
    "CURRENT_QUERY", "DEPTH", "DEPTH24_STENCIL8", "DEPTH32F_STENCIL8", "DEPTH_COMPONENT24",
    "DEPTH_COMPONENT32F", "DRAW_BUFFER0", "DRAW_BUFFER1", "DRAW_BUFFER10", "DRAW_BUFFER11", "DRAW_BUFFER12",
    "DRAW_BUFFER13", "DRAW_BUFFER14", "DRAW_BUFFER15", "DRAW_BUFFER2", "DRAW_BUFFER3", "DRAW_BUFFER4",
    "DRAW_BUFFER5", "DRAW_BUFFER6", "DRAW_BUFFER7", "DRAW_BUFFER8", "DRAW_BUFFER9", "DRAW_FRAMEBUFFER",
    "DRAW_FRAMEBUFFER_BINDING", "DYNAMIC_COPY", "DYNAMIC_READ", "FLOAT_32_UNSIGNED_INT_24_8_REV",
    "FLOAT_MAT2x3", "FLOAT_MAT2x4", "FLOAT_MAT3x2", "FLOAT_MAT3x4", "FLOAT_MAT4x2", "FLOAT_MAT4x3",
    "FRAGMENT_SHADER_DERIVATIVE_HINT", "FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE",
    "FRAMEBUFFER_ATTACHMENT_BLUE_SIZE", "FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING",
    "FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE", "FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE",
    "FRAMEBUFFER_ATTACHMENT_GREEN_SIZE", "FRAMEBUFFER_ATTACHMENT_RED_SIZE",
    "FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE", "FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER", "FRAMEBUFFER_DEFAULT",
    "FRAMEBUFFER_INCOMPLETE_MULTISAMPLE", "HALF_FLOAT", "INTERLEAVED_ATTRIBS", "INT_2_10_10_10_REV",
    "INT_SAMPLER_2D", "INT_SAMPLER_2D_ARRAY", "INT_SAMPLER_3D", "INT_SAMPLER_CUBE", "INVALID_INDEX", "MAX",
    "MAX_3D_TEXTURE_SIZE", "MAX_ARRAY_TEXTURE_LAYERS", "MAX_CLIENT_WAIT_TIMEOUT_WEBGL",
    "MAX_COLOR_ATTACHMENTS", "MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS", "MAX_COMBINED_UNIFORM_BLOCKS",
    "MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS", "MAX_DRAW_BUFFERS", "MAX_ELEMENTS_INDICES",
    "MAX_ELEMENTS_VERTICES", "MAX_ELEMENT_INDEX", "MAX_FRAGMENT_INPUT_COMPONENTS",
    "MAX_FRAGMENT_UNIFORM_BLOCKS", "MAX_FRAGMENT_UNIFORM_COMPONENTS", "MAX_PROGRAM_TEXEL_OFFSET",
    "MAX_SAMPLES", "MAX_SERVER_WAIT_TIMEOUT", "MAX_TEXTURE_LOD_BIAS",
    "MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS", "MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS",
    "MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS", "MAX_UNIFORM_BLOCK_SIZE", "MAX_UNIFORM_BUFFER_BINDINGS",
    "MAX_VARYING_COMPONENTS", "MAX_VERTEX_OUTPUT_COMPONENTS", "MAX_VERTEX_UNIFORM_BLOCKS",
    "MAX_VERTEX_UNIFORM_COMPONENTS", "MIN", "MIN_PROGRAM_TEXEL_OFFSET", "OBJECT_TYPE", "PACK_ROW_LENGTH",
    "PACK_SKIP_PIXELS", "PACK_SKIP_ROWS", "PIXEL_PACK_BUFFER", "PIXEL_PACK_BUFFER_BINDING",
    "PIXEL_UNPACK_BUFFER", "PIXEL_UNPACK_BUFFER_BINDING", "QUERY_RESULT", "QUERY_RESULT_AVAILABLE",
    "R11F_G11F_B10F", "R16F", "R16I", "R16UI", "R32F", "R32I", "R32UI", "R8", "R8I", "R8UI", "R8_SNORM",
    "RASTERIZER_DISCARD", "READ_BUFFER", "READ_FRAMEBUFFER", "READ_FRAMEBUFFER_BINDING", "RED", "RED_INTEGER",
    "RENDERBUFFER_SAMPLES", "RG", "RG16F", "RG16I", "RG16UI", "RG32F", "RG32I", "RG32UI", "RG8", "RG8I",
    "RG8UI", "RG8_SNORM", "RGB10_A2", "RGB10_A2UI", "RGB16F", "RGB16I", "RGB16UI", "RGB32F", "RGB32I",
    "RGB32UI", "RGB8I", "RGB8UI", "RGB8_SNORM", "RGB9_E5", "RGBA16F", "RGBA16I", "RGBA16UI", "RGBA32F",
    "RGBA32I", "RGBA32UI", "RGBA8I", "RGBA8UI", "RGBA8_SNORM", "RGBA_INTEGER", "RGB_INTEGER", "RG_INTEGER",
    "SAMPLER_2D_ARRAY", "SAMPLER_2D_ARRAY_SHADOW", "SAMPLER_2D_SHADOW", "SAMPLER_3D", "SAMPLER_BINDING",
    "SAMPLER_CUBE_SHADOW", "SEPARATE_ATTRIBS", "SIGNALED", "SIGNED_NORMALIZED", "SRGB", "SRGB8",
    "SRGB8_ALPHA8", "STATIC_COPY", "STATIC_READ", "STENCIL", "STREAM_COPY", "STREAM_READ", "SYNC_CONDITION",
    "SYNC_FENCE", "SYNC_FLAGS", "SYNC_FLUSH_COMMANDS_BIT", "SYNC_GPU_COMMANDS_COMPLETE", "SYNC_STATUS",
    "TEXTURE_2D_ARRAY", "TEXTURE_3D", "TEXTURE_BASE_LEVEL", "TEXTURE_BINDING_2D_ARRAY", "TEXTURE_BINDING_3D",
    "TEXTURE_COMPARE_FUNC", "TEXTURE_COMPARE_MODE", "TEXTURE_IMMUTABLE_FORMAT", "TEXTURE_IMMUTABLE_LEVELS",
    "TEXTURE_MAX_LEVEL", "TEXTURE_MAX_LOD", "TEXTURE_MIN_LOD", "TEXTURE_WRAP_R", "TIMEOUT_EXPIRED",
    "TIMEOUT_IGNORED", "TRANSFORM_FEEDBACK", "TRANSFORM_FEEDBACK_ACTIVE", "TRANSFORM_FEEDBACK_BINDING",
    "TRANSFORM_FEEDBACK_BUFFER", "TRANSFORM_FEEDBACK_BUFFER_BINDING", "TRANSFORM_FEEDBACK_BUFFER_MODE",
    "TRANSFORM_FEEDBACK_BUFFER_SIZE", "TRANSFORM_FEEDBACK_BUFFER_START", "TRANSFORM_FEEDBACK_PAUSED",
    "TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN", "TRANSFORM_FEEDBACK_VARYINGS", "UNIFORM_ARRAY_STRIDE",
    "UNIFORM_BLOCK_ACTIVE_UNIFORMS", "UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES", "UNIFORM_BLOCK_BINDING",
    "UNIFORM_BLOCK_DATA_SIZE", "UNIFORM_BLOCK_INDEX", "UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER",
    "UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER", "UNIFORM_BUFFER", "UNIFORM_BUFFER_BINDING",
    "UNIFORM_BUFFER_OFFSET_ALIGNMENT", "UNIFORM_BUFFER_SIZE", "UNIFORM_BUFFER_START", "UNIFORM_IS_ROW_MAJOR",
    "UNIFORM_MATRIX_STRIDE", "UNIFORM_OFFSET", "UNIFORM_SIZE", "UNIFORM_TYPE", "UNPACK_IMAGE_HEIGHT",
    "UNPACK_ROW_LENGTH", "UNPACK_SKIP_IMAGES", "UNPACK_SKIP_PIXELS", "UNPACK_SKIP_ROWS", "UNSIGNALED",
    "UNSIGNED_INT_10F_11F_11F_REV", "UNSIGNED_INT_24_8", "UNSIGNED_INT_2_10_10_10_REV",
    "UNSIGNED_INT_5_9_9_9_REV", "UNSIGNED_INT_SAMPLER_2D", "UNSIGNED_INT_SAMPLER_2D_ARRAY",
    "UNSIGNED_INT_SAMPLER_3D", "UNSIGNED_INT_SAMPLER_CUBE", "UNSIGNED_INT_VEC2", "UNSIGNED_INT_VEC3",
    "UNSIGNED_INT_VEC4", "UNSIGNED_NORMALIZED", "VERTEX_ARRAY_BINDING", "VERTEX_ATTRIB_ARRAY_DIVISOR",
    "VERTEX_ATTRIB_ARRAY_INTEGER", "WAIT_FAILED", "beginQuery", "beginTransformFeedback", "bindBufferBase",
    "bindBufferRange", "bindSampler", "bindTransformFeedback", "bindVertexArray", "blitFramebuffer",
    "clearBufferfi", "clearBufferfv", "clearBufferiv", "clearBufferuiv", "clientWaitSync",
    "compressedTexImage3D", "compressedTexSubImage3D", "copyBufferSubData", "copyTexSubImage3D",
    "createQuery", "createSampler", "createTransformFeedback", "createVertexArray", "deleteQuery",
    "deleteSampler", "deleteSync", "deleteTransformFeedback", "deleteVertexArray", "drawArraysInstanced",
    "drawBuffers", "drawElementsInstanced", "drawRangeElements", "endQuery", "endTransformFeedback",
    "fenceSync", "framebufferTextureLayer", "getActiveUniformBlockName", "getActiveUniformBlockParameter",
    "getActiveUniforms", "getBufferSubData", "getFragDataLocation", "getIndexedParameter",
    "getInternalformatParameter", "getQuery", "getQueryParameter", "getSamplerParameter", "getSyncParameter",
    "getTransformFeedbackVarying", "getUniformBlockIndex", "getUniformIndices", "invalidateFramebuffer",
    "invalidateSubFramebuffer", "isQuery", "isSampler", "isSync", "isTransformFeedback", "isVertexArray",
    "pauseTransformFeedback", "readBuffer", "renderbufferStorageMultisample", "resumeTransformFeedback",
    "samplerParameterf", "samplerParameteri", "texImage3D", "texStorage2D", "texStorage3D", "texSubImage3D",
    "transformFeedbackVaryings", "uniform1ui", "uniform1uiv", "uniform2ui", "uniform2uiv", "uniform3ui",
    "uniform3uiv", "uniform4ui", "uniform4uiv", "uniformBlockBinding", "uniformMatrix2x3fv",
    "uniformMatrix2x4fv", "uniformMatrix3x2fv", "uniformMatrix3x4fv", "uniformMatrix4x2fv",
    "uniformMatrix4x3fv", "vertexAttribDivisor", "vertexAttribI4i", "vertexAttribI4iv", "vertexAttribI4ui",
    "vertexAttribI4uiv", "vertexAttribIPointer", "waitSync",
};

// bro's own WebGL 2 additions on the prototype: BRO_buffer_map's mapping
// calls (buffer mapping is WebGL 2 functionality) and the anisotropy enum
// EXT_texture_filter_anisotropic carries.
constexpr std::string_view kBroWebGL2Only[] = {"TEXTURE_MAX_ANISOTROPY_EXT", "flushMappedBufferRange",
                                               "mapBufferRange", "unmapBuffer"};

}  // namespace

bool isWebGL2OnlyName(std::string_view name) {
    return std::binary_search(std::begin(kWebGL2Only), std::end(kWebGL2Only), name) ||
           std::find(std::begin(kBroWebGL2Only), std::end(kBroWebGL2Only), name) != std::end(kBroWebGL2Only);
}

void removeWebGL2OnlyMethods(Value proto) {
    // Constants are installed filtered (they are not configurable); methods
    // and accessors are deleted here.
    const auto drop = [proto](std::string_view name) {
        if (!name.empty() && !(name[0] >= 'A' && name[0] <= 'Z')) ev::deleteProperty(proto, name);
    };
    for (std::string_view name : kWebGL2Only) drop(name);
    for (std::string_view name : kBroWebGL2Only) drop(name);
}

void webgl1TexFormat(webgl::WebGL2RenderingContext* c, uint32_t* internalformat, uint32_t* format, uint32_t* type) {
    if (!c || c->version() != 1) return;
    constexpr uint32_t kHalfFloatOES = 0x8D61, kSrgbExt = 0x8C40, kSrgbAlphaExt = 0x8C42;
    if (type && *type == kHalfFloatOES) *type = 0x140B;  // HALF_FLOAT
    // EXT_sRGB: the unsized sRGB formats are WebGL 2's sized ones, with the
    // client data RGB / RGBA.
    const auto srgb = [](uint32_t* f, bool sized) {
        if (!f) return;
        if (*f == kSrgbExt) *f = sized ? 0x8C41 : 0x1907;            // SRGB8 / RGB
        else if (*f == kSrgbAlphaExt) *f = sized ? 0x8C43 : 0x1908;  // SRGB8_ALPHA8 / RGBA
    };
    srgb(internalformat, true);
    srgb(format, false);
}

bool webgl1Refuses(webgl::WebGL2RenderingContext* c, WebGL1Enum kind, uint32_t v) {
    if (!c || c->version() != 1) return false;
    const auto ext = [c](const char* name) { return webglExtensionEnabled(c, name); };
    bool ok = true;
    GLenum error = GL_INVALID_ENUM;
    switch (kind) {
        case WebGL1Enum::Pname:
            if (v == 0x85B5) ok = ext("OES_vertex_array_object");  // VERTEX_ARRAY_BINDING_OES
            else if (v == 0x8824 || v == 0x8CDF || (v >= 0x8825 && v <= 0x8834)) ok = ext("WEBGL_draw_buffers");
            else if (v == 0x8B8B) ok = ext("OES_standard_derivatives");
            else ok = !isWebGL2OnlyEnum(v);
            break;
        case WebGL1Enum::Cap:
        case WebGL1Enum::PixelStore:
        case WebGL1Enum::TexParameter: ok = !isWebGL2OnlyEnum(v); break;
        case WebGL1Enum::BufferTarget: ok = v == 0x8892 || v == 0x8893; break;     // ARRAY / ELEMENT_ARRAY
        case WebGL1Enum::TextureTarget: ok = v == 0x0DE1 || v == 0x8513; break;    // 2D / CUBE_MAP
        case WebGL1Enum::FramebufferTarget: ok = v == 0x8D40; break;               // FRAMEBUFFER
        case WebGL1Enum::BlendEquation: ok = (v != 0x8007 && v != 0x8008) || ext("EXT_blend_minmax"); break;
        case WebGL1Enum::Hint: ok = v != 0x8B8B || ext("OES_standard_derivatives"); break;
        case WebGL1Enum::RenderbufferFormat:
            // RGBA4, RGB565, RGB5_A1, DEPTH_COMPONENT16, STENCIL_INDEX8, DEPTH_STENCIL.
            ok = v == 0x8056 || v == 0x8D62 || v == 0x8057 || v == 0x81A5 || v == 0x8D48 || v == 0x84F9 ||
                 (v == 0x8C43 && ext("EXT_sRGB")) || (v == 0x8814 && ext("WEBGL_color_buffer_float")) ||
                 ((v == 0x881A || v == 0x881B) && ext("EXT_color_buffer_half_float"));
            break;
        case WebGL1Enum::TexType:
            // UNSIGNED_BYTE and the packed 16-bit types; the rest with their
            // extensions.
            ok = v == 0x1401 || v == 0x8363 || v == 0x8033 || v == 0x8034 ||
                 (v == 0x1406 && ext("OES_texture_float")) || (v == 0x8D61 && ext("OES_texture_half_float")) ||
                 ((v == 0x1403 || v == 0x1405 || v == 0x84FA) && ext("WEBGL_depth_texture"));
            break;
        case WebGL1Enum::TexInternalFormat:
            // ES 2.0's unsized formats (internalformat names the format),
            // the depth ones and EXT_sRGB's; anything else is INVALID_VALUE.
            ok = v == 0x1906 || v == 0x1907 || v == 0x1908 || v == 0x1909 || v == 0x190A ||  // ALPHA..LUMINANCE_ALPHA
                 v == 0x1902 || v == 0x84F9 ||                                             // DEPTH_COMPONENT, DEPTH_STENCIL
                 v == 0x8C40 || v == 0x8C42;                                               // SRGB_EXT, SRGB_ALPHA_EXT
            error = GL_INVALID_VALUE;
            break;
    }
    if (ok) return false;
    if (auto* cl = client(c)) cl->syntheticError(error);
    return true;
}

bool makeWebGL1Extension(webgl::WebGL2RenderingContext* c, const std::string& name, ObjectBuilder& o) {
    auto def = [&o](const char* n, double v) { o.set(n, ev::fromDouble(v)); };
    if (name == "ANGLE_instanced_arrays") {
        def("VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE", 0x88FE);
        o.def("drawArraysInstancedANGLE", 4, [c](Value, std::span<const Value> a) {
            if (auto* cl = client(c)) cl->drawArraysInstanced(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
            return ev::undefined();
        });
        o.def("drawElementsInstancedANGLE", 5, [c](Value, std::span<const Value> a) {
            if (auto* cl = client(c))
                cl->drawElementsInstanced(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                          static_cast<uintptr_t>(i64At(a, 3)), i32At(a, 4));
            return ev::undefined();
        });
        o.def("vertexAttribDivisorANGLE", 2, [c](Value, std::span<const Value> a) {
            if (auto* cl = client(c)) cl->vertexAttribDivisor(u32At(a, 0), u32At(a, 1));
            return ev::undefined();
        });
    } else if (name == "OES_vertex_array_object") {
        def("VERTEX_ARRAY_BINDING_OES", 0x85B5);
        o.def("createVertexArrayOES", 0, [c](Value, std::span<const Value>) {
            auto* cl = client(c);
            if (!cl) return ev::null();
            const GLuint id = c->newObjectId();
            cl->createVertexArray(id);
            return webglObject(c, WebGLCell::VertexArray, id);
        });
        o.def("deleteVertexArrayOES", 1, [c](Value, std::span<const Value> a) {
            const GLuint id = idOf(argAt(a, 0), WebGLCell::VertexArray);
            if (auto* cl = client(c)) cl->deleteVertexArray(webgl::WebGLVertexArrayObject{id});
            forgetWebGLObject(c, WebGLCell::VertexArray, id);
            return ev::undefined();
        });
        o.def("isVertexArrayOES", 1, [c](Value, std::span<const Value> a) {
            auto* cl = client(c);
            const webgl::WebGLVertexArrayObject vao{idOf(argAt(a, 0), WebGLCell::VertexArray)};
            return ev::fromBool(cl && cl->sync([vao](WebGLBackend& gl) { return gl.isVertexArray(vao); }) != GL_FALSE);
        });
        o.def("bindVertexArrayOES", 1, [c](Value, std::span<const Value> a) {
            if (auto* cl = client(c))
                cl->bindVertexArray(webgl::WebGLVertexArrayObject{idOf(argAt(a, 0), WebGLCell::VertexArray)});
            return ev::undefined();
        });
    } else if (name == "WEBGL_draw_buffers") {
        static constexpr const char* kAttach[] = {
            "COLOR_ATTACHMENT0_WEBGL", "COLOR_ATTACHMENT1_WEBGL", "COLOR_ATTACHMENT2_WEBGL", "COLOR_ATTACHMENT3_WEBGL",
            "COLOR_ATTACHMENT4_WEBGL", "COLOR_ATTACHMENT5_WEBGL", "COLOR_ATTACHMENT6_WEBGL", "COLOR_ATTACHMENT7_WEBGL",
            "COLOR_ATTACHMENT8_WEBGL", "COLOR_ATTACHMENT9_WEBGL", "COLOR_ATTACHMENT10_WEBGL", "COLOR_ATTACHMENT11_WEBGL",
            "COLOR_ATTACHMENT12_WEBGL", "COLOR_ATTACHMENT13_WEBGL", "COLOR_ATTACHMENT14_WEBGL", "COLOR_ATTACHMENT15_WEBGL"};
        static constexpr const char* kDraw[] = {
            "DRAW_BUFFER0_WEBGL", "DRAW_BUFFER1_WEBGL", "DRAW_BUFFER2_WEBGL", "DRAW_BUFFER3_WEBGL",
            "DRAW_BUFFER4_WEBGL", "DRAW_BUFFER5_WEBGL", "DRAW_BUFFER6_WEBGL", "DRAW_BUFFER7_WEBGL",
            "DRAW_BUFFER8_WEBGL", "DRAW_BUFFER9_WEBGL", "DRAW_BUFFER10_WEBGL", "DRAW_BUFFER11_WEBGL",
            "DRAW_BUFFER12_WEBGL", "DRAW_BUFFER13_WEBGL", "DRAW_BUFFER14_WEBGL", "DRAW_BUFFER15_WEBGL"};
        for (int i = 0; i < 16; ++i) def(kAttach[i], 0x8CE0 + i);
        for (int i = 0; i < 16; ++i) def(kDraw[i], 0x8825 + i);
        def("MAX_COLOR_ATTACHMENTS_WEBGL", 0x8CDF);
        def("MAX_DRAW_BUFFERS_WEBGL", 0x8824);
        o.def("drawBuffersWEBGL", 1, [c](Value, std::span<const Value> a) {
            SmallBuffer<uint32_t, 16> storage;
            const uint32_t* p = nullptr;
            size_t n = 0;
            if (uint32Data(argAt(a, 0), storage, &p, &n) && n > 0) {
                if (auto* cl = client(c))
                    cl->drawBuffers(static_cast<GLsizei>(n), reinterpret_cast<const GLenum*>(p), n);
            }
            return ev::undefined();
        });
    } else if (name == "OES_texture_half_float") {
        def("HALF_FLOAT_OES", 0x8D61);
    } else if (name == "OES_standard_derivatives") {
        def("FRAGMENT_SHADER_DERIVATIVE_HINT_OES", 0x8B8B);
    } else if (name == "WEBGL_depth_texture") {
        def("UNSIGNED_INT_24_8_WEBGL", 0x84FA);
    } else if (name == "EXT_blend_minmax") {
        def("MIN_EXT", 0x8007);
        def("MAX_EXT", 0x8008);
    } else if (name == "EXT_sRGB") {
        def("SRGB_EXT", 0x8C40);
        def("SRGB_ALPHA_EXT", 0x8C42);
        def("SRGB8_ALPHA8_EXT", 0x8C43);
        def("FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT", 0x8210);
    } else if (name == "WEBGL_color_buffer_float") {
        def("RGBA32F_EXT", 0x8814);
        def("FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", 0x8211);
        def("UNSIGNED_NORMALIZED_EXT", 0x8C17);
    } else {
        return false;
    }
    return true;
}

}  // namespace bro::bronze_host

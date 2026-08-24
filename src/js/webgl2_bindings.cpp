#include "js/webgl2_bindings.h"
#include "js/webgl2_bindings_util.h"
#include "webgl/webgl2_context.h"
#include "webgl/webgl_objects.h"
#include "image_gpu.js.h"
#include <glad/gl.h>
#include <cstring>
#include <qjsbind/qjsbind.h>

namespace bro::js {


using namespace webgl2;

// ===========================================================================
// Class IDs (synced from qjsbind after registration)
// ===========================================================================

namespace webgl2 {
    JSClassID js_webgl2_ctx_class_id = 0;
    JSClassID js_webgl_buffer_class_id = 0;
    JSClassID js_webgl_texture_class_id = 0;
    JSClassID js_webgl_program_class_id = 0;
    JSClassID js_webgl_shader_class_id = 0;
    JSClassID js_webgl_framebuffer_class_id = 0;
    JSClassID js_webgl_renderbuffer_class_id = 0;
    JSClassID js_webgl_vao_class_id = 0;
    JSClassID js_webgl_uniform_loc_class_id = 0;
    JSClassID js_webgl_sampler_class_id = 0;
    JSClassID js_webgl_query_class_id = 0;
    JSClassID js_webgl_sync_class_id = 0;
    JSClassID js_webgl_tf_class_id = 0;
}

// ===========================================================================
// Object wrapping implementations
// ===========================================================================

namespace webgl2 {

#define IMPL_WRAP(Name, CppType, classId)                                       \
    JSValue wrap##Name(JSContext* ctx, CppType obj) {                            \
        return qjsbind::wrap<CppType>(ctx, new CppType(obj));                   \
    }

IMPL_WRAP(Buffer, bro::webgl::WebGLBuffer, js_webgl_buffer_class_id)
IMPL_WRAP(Texture, bro::webgl::WebGLTexture, js_webgl_texture_class_id)
IMPL_WRAP(Program, bro::webgl::WebGLProgram, js_webgl_program_class_id)
IMPL_WRAP(Shader, bro::webgl::WebGLShader, js_webgl_shader_class_id)
IMPL_WRAP(Framebuffer, bro::webgl::WebGLFramebuffer, js_webgl_framebuffer_class_id)
IMPL_WRAP(Renderbuffer, bro::webgl::WebGLRenderbuffer, js_webgl_renderbuffer_class_id)
IMPL_WRAP(VAO, bro::webgl::WebGLVertexArrayObject, js_webgl_vao_class_id)
IMPL_WRAP(UniformLocation, bro::webgl::WebGLUniformLocation, js_webgl_uniform_loc_class_id)
IMPL_WRAP(Sampler, bro::webgl::WebGLSampler, js_webgl_sampler_class_id)
IMPL_WRAP(Query, bro::webgl::WebGLQuery, js_webgl_query_class_id)
IMPL_WRAP(Sync, bro::webgl::WebGLSync, js_webgl_sync_class_id)
IMPL_WRAP(TransformFeedback, bro::webgl::WebGLTransformFeedback, js_webgl_tf_class_id)

#undef IMPL_WRAP

} // namespace webgl2

// ===========================================================================
// WebGL2 constants (subset covering all commonly used enums)
// ===========================================================================

static const JSCFunctionListEntry webgl2_constants[] = {
    // Clear bits
    JS_PROP_INT32_DEF("DEPTH_BUFFER_BIT", 0x00000100, 0),
    JS_PROP_INT32_DEF("STENCIL_BUFFER_BIT", 0x00000400, 0),
    JS_PROP_INT32_DEF("COLOR_BUFFER_BIT", 0x00004000, 0),

    // Boolean
    JS_PROP_INT32_DEF("FALSE", 0, 0),
    JS_PROP_INT32_DEF("TRUE", 1, 0),

    // Data types
    JS_PROP_INT32_DEF("BYTE", 0x1400, 0),
    JS_PROP_INT32_DEF("UNSIGNED_BYTE", 0x1401, 0),
    JS_PROP_INT32_DEF("SHORT", 0x1402, 0),
    JS_PROP_INT32_DEF("UNSIGNED_SHORT", 0x1403, 0),
    JS_PROP_INT32_DEF("INT", 0x1404, 0),
    JS_PROP_INT32_DEF("UNSIGNED_INT", 0x1405, 0),
    JS_PROP_INT32_DEF("FLOAT", 0x1406, 0),
    JS_PROP_INT32_DEF("HALF_FLOAT", 0x140B, 0),

    // Primitives
    JS_PROP_INT32_DEF("POINTS", 0x0000, 0),
    JS_PROP_INT32_DEF("LINES", 0x0001, 0),
    JS_PROP_INT32_DEF("LINE_LOOP", 0x0002, 0),
    JS_PROP_INT32_DEF("LINE_STRIP", 0x0003, 0),
    JS_PROP_INT32_DEF("TRIANGLES", 0x0004, 0),
    JS_PROP_INT32_DEF("TRIANGLE_STRIP", 0x0005, 0),
    JS_PROP_INT32_DEF("TRIANGLE_FAN", 0x0006, 0),

    // Blend
    JS_PROP_INT32_DEF("ZERO", 0, 0),
    JS_PROP_INT32_DEF("ONE", 1, 0),
    JS_PROP_INT32_DEF("SRC_COLOR", 0x0300, 0),
    JS_PROP_INT32_DEF("ONE_MINUS_SRC_COLOR", 0x0301, 0),
    JS_PROP_INT32_DEF("SRC_ALPHA", 0x0302, 0),
    JS_PROP_INT32_DEF("ONE_MINUS_SRC_ALPHA", 0x0303, 0),
    JS_PROP_INT32_DEF("DST_ALPHA", 0x0304, 0),
    JS_PROP_INT32_DEF("ONE_MINUS_DST_ALPHA", 0x0305, 0),
    JS_PROP_INT32_DEF("DST_COLOR", 0x0306, 0),
    JS_PROP_INT32_DEF("ONE_MINUS_DST_COLOR", 0x0307, 0),
    JS_PROP_INT32_DEF("SRC_ALPHA_SATURATE", 0x0308, 0),
    JS_PROP_INT32_DEF("CONSTANT_COLOR", 0x8001, 0),
    JS_PROP_INT32_DEF("ONE_MINUS_CONSTANT_COLOR", 0x8002, 0),
    JS_PROP_INT32_DEF("CONSTANT_ALPHA", 0x8003, 0),
    JS_PROP_INT32_DEF("ONE_MINUS_CONSTANT_ALPHA", 0x8004, 0),

    // Blend equations
    JS_PROP_INT32_DEF("FUNC_ADD", 0x8006, 0),
    JS_PROP_INT32_DEF("FUNC_SUBTRACT", 0x800A, 0),
    JS_PROP_INT32_DEF("FUNC_REVERSE_SUBTRACT", 0x800B, 0),
    JS_PROP_INT32_DEF("MIN", 0x8007, 0),
    JS_PROP_INT32_DEF("MAX", 0x8008, 0),

    // Buffer targets
    JS_PROP_INT32_DEF("ARRAY_BUFFER", 0x8892, 0),
    JS_PROP_INT32_DEF("ELEMENT_ARRAY_BUFFER", 0x8893, 0),
    JS_PROP_INT32_DEF("COPY_READ_BUFFER", 0x8F36, 0),
    JS_PROP_INT32_DEF("COPY_WRITE_BUFFER", 0x8F37, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_BUFFER", 0x8C8E, 0),
    JS_PROP_INT32_DEF("UNIFORM_BUFFER", 0x8A11, 0),
    JS_PROP_INT32_DEF("PIXEL_PACK_BUFFER", 0x88EB, 0),
    JS_PROP_INT32_DEF("PIXEL_UNPACK_BUFFER", 0x88EC, 0),
    JS_PROP_INT32_DEF("PIXEL_PACK_BUFFER_BINDING", 0x88ED, 0),
    JS_PROP_INT32_DEF("PIXEL_UNPACK_BUFFER_BINDING", 0x88EF, 0),

    // Buffer usage
    JS_PROP_INT32_DEF("STREAM_DRAW", 0x88E0, 0),
    JS_PROP_INT32_DEF("STREAM_READ", 0x88E1, 0),
    JS_PROP_INT32_DEF("STREAM_COPY", 0x88E2, 0),
    JS_PROP_INT32_DEF("STATIC_DRAW", 0x88E4, 0),
    JS_PROP_INT32_DEF("STATIC_READ", 0x88E5, 0),
    JS_PROP_INT32_DEF("STATIC_COPY", 0x88E6, 0),
    JS_PROP_INT32_DEF("DYNAMIC_DRAW", 0x88E8, 0),
    JS_PROP_INT32_DEF("DYNAMIC_READ", 0x88E9, 0),
    JS_PROP_INT32_DEF("DYNAMIC_COPY", 0x88EA, 0),

    // Depth/stencil
    JS_PROP_INT32_DEF("DEPTH_TEST", 0x0B71, 0),
    JS_PROP_INT32_DEF("STENCIL_TEST", 0x0B90, 0),
    JS_PROP_INT32_DEF("SCISSOR_TEST", 0x0C11, 0),
    JS_PROP_INT32_DEF("NEVER", 0x0200, 0),
    JS_PROP_INT32_DEF("LESS", 0x0201, 0),
    JS_PROP_INT32_DEF("EQUAL", 0x0202, 0),
    JS_PROP_INT32_DEF("LEQUAL", 0x0203, 0),
    JS_PROP_INT32_DEF("GREATER", 0x0204, 0),
    JS_PROP_INT32_DEF("NOTEQUAL", 0x0205, 0),
    JS_PROP_INT32_DEF("GEQUAL", 0x0206, 0),
    JS_PROP_INT32_DEF("ALWAYS", 0x0207, 0),
    JS_PROP_INT32_DEF("KEEP", 0x1E00, 0),
    JS_PROP_INT32_DEF("REPLACE", 0x1E01, 0),
    JS_PROP_INT32_DEF("INCR", 0x1E02, 0),
    JS_PROP_INT32_DEF("DECR", 0x1E03, 0),
    JS_PROP_INT32_DEF("INVERT", 0x150A, 0),
    JS_PROP_INT32_DEF("INCR_WRAP", 0x8507, 0),
    JS_PROP_INT32_DEF("DECR_WRAP", 0x8508, 0),

    // Enable/disable caps
    JS_PROP_INT32_DEF("BLEND", 0x0BE2, 0),
    JS_PROP_INT32_DEF("DITHER", 0x0BD0, 0),
    JS_PROP_INT32_DEF("CULL_FACE", 0x0B44, 0),
    JS_PROP_INT32_DEF("POLYGON_OFFSET_FILL", 0x8037, 0),
    JS_PROP_INT32_DEF("SAMPLE_ALPHA_TO_COVERAGE", 0x809E, 0),
    JS_PROP_INT32_DEF("SAMPLE_COVERAGE", 0x80A0, 0),
    JS_PROP_INT32_DEF("RASTERIZER_DISCARD", 0x8C89, 0),

    // Face culling
    JS_PROP_INT32_DEF("FRONT", 0x0404, 0),
    JS_PROP_INT32_DEF("BACK", 0x0405, 0),
    JS_PROP_INT32_DEF("FRONT_AND_BACK", 0x0408, 0),
    JS_PROP_INT32_DEF("CW", 0x0900, 0),
    JS_PROP_INT32_DEF("CCW", 0x0901, 0),

    // Textures
    JS_PROP_INT32_DEF("TEXTURE_2D", 0x0DE1, 0),
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP", 0x8513, 0),
    JS_PROP_INT32_DEF("TEXTURE_3D", 0x806F, 0),
    JS_PROP_INT32_DEF("TEXTURE_2D_ARRAY", 0x8C1A, 0),
    JS_PROP_INT32_DEF("TEXTURE0",  0x84C0, 0),
    JS_PROP_INT32_DEF("TEXTURE1",  0x84C1, 0),
    JS_PROP_INT32_DEF("TEXTURE2",  0x84C2, 0),
    JS_PROP_INT32_DEF("TEXTURE3",  0x84C3, 0),
    JS_PROP_INT32_DEF("TEXTURE4",  0x84C4, 0),
    JS_PROP_INT32_DEF("TEXTURE5",  0x84C5, 0),
    JS_PROP_INT32_DEF("TEXTURE6",  0x84C6, 0),
    JS_PROP_INT32_DEF("TEXTURE7",  0x84C7, 0),
    JS_PROP_INT32_DEF("TEXTURE8",  0x84C8, 0),
    JS_PROP_INT32_DEF("TEXTURE9",  0x84C9, 0),
    JS_PROP_INT32_DEF("TEXTURE10", 0x84CA, 0),
    JS_PROP_INT32_DEF("TEXTURE11", 0x84CB, 0),
    JS_PROP_INT32_DEF("TEXTURE12", 0x84CC, 0),
    JS_PROP_INT32_DEF("TEXTURE13", 0x84CD, 0),
    JS_PROP_INT32_DEF("TEXTURE14", 0x84CE, 0),
    JS_PROP_INT32_DEF("TEXTURE15", 0x84CF, 0),
    JS_PROP_INT32_DEF("TEXTURE16", 0x84D0, 0),
    JS_PROP_INT32_DEF("TEXTURE17", 0x84D1, 0),
    JS_PROP_INT32_DEF("TEXTURE18", 0x84D2, 0),
    JS_PROP_INT32_DEF("TEXTURE19", 0x84D3, 0),
    JS_PROP_INT32_DEF("TEXTURE20", 0x84D4, 0),
    JS_PROP_INT32_DEF("TEXTURE21", 0x84D5, 0),
    JS_PROP_INT32_DEF("TEXTURE22", 0x84D6, 0),
    JS_PROP_INT32_DEF("TEXTURE23", 0x84D7, 0),
    JS_PROP_INT32_DEF("TEXTURE24", 0x84D8, 0),
    JS_PROP_INT32_DEF("TEXTURE25", 0x84D9, 0),
    JS_PROP_INT32_DEF("TEXTURE26", 0x84DA, 0),
    JS_PROP_INT32_DEF("TEXTURE27", 0x84DB, 0),
    JS_PROP_INT32_DEF("TEXTURE28", 0x84DC, 0),
    JS_PROP_INT32_DEF("TEXTURE29", 0x84DD, 0),
    JS_PROP_INT32_DEF("TEXTURE30", 0x84DE, 0),
    JS_PROP_INT32_DEF("TEXTURE31", 0x84DF, 0),
    JS_PROP_INT32_DEF("TEXTURE_MAG_FILTER", 0x2800, 0),
    JS_PROP_INT32_DEF("TEXTURE_MIN_FILTER", 0x2801, 0),
    JS_PROP_INT32_DEF("TEXTURE_WRAP_S", 0x2802, 0),
    JS_PROP_INT32_DEF("TEXTURE_WRAP_T", 0x2803, 0),
    JS_PROP_INT32_DEF("TEXTURE_WRAP_R", 0x8072, 0),
    JS_PROP_INT32_DEF("TEXTURE_COMPARE_MODE", 0x884C, 0),
    JS_PROP_INT32_DEF("TEXTURE_COMPARE_FUNC", 0x884D, 0),
    JS_PROP_INT32_DEF("TEXTURE_MAX_LEVEL", 0x813D, 0),
    JS_PROP_INT32_DEF("TEXTURE_BASE_LEVEL", 0x813C, 0),
    JS_PROP_INT32_DEF("TEXTURE_MIN_LOD", 0x813A, 0),
    JS_PROP_INT32_DEF("TEXTURE_MAX_LOD", 0x813B, 0),
    JS_PROP_INT32_DEF("TEXTURE_MAX_ANISOTROPY_EXT", 0x84FE, 0),
    JS_PROP_INT32_DEF("COMPARE_REF_TO_TEXTURE", 0x884E, 0),
    JS_PROP_INT32_DEF("NEAREST", 0x2600, 0),
    JS_PROP_INT32_DEF("LINEAR", 0x2601, 0),
    JS_PROP_INT32_DEF("NEAREST_MIPMAP_NEAREST", 0x2700, 0),
    JS_PROP_INT32_DEF("LINEAR_MIPMAP_NEAREST", 0x2701, 0),
    JS_PROP_INT32_DEF("NEAREST_MIPMAP_LINEAR", 0x2702, 0),
    JS_PROP_INT32_DEF("LINEAR_MIPMAP_LINEAR", 0x2703, 0),
    JS_PROP_INT32_DEF("REPEAT", 0x2901, 0),
    JS_PROP_INT32_DEF("CLAMP_TO_EDGE", 0x812F, 0),
    JS_PROP_INT32_DEF("MIRRORED_REPEAT", 0x8370, 0),

    // Texture cube map faces
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP_POSITIVE_X", 0x8515, 0),
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP_NEGATIVE_X", 0x8516, 0),
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP_POSITIVE_Y", 0x8517, 0),
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP_NEGATIVE_Y", 0x8518, 0),
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP_POSITIVE_Z", 0x8519, 0),
    JS_PROP_INT32_DEF("TEXTURE_CUBE_MAP_NEGATIVE_Z", 0x851A, 0),

    // Pixel formats
    JS_PROP_INT32_DEF("ALPHA", 0x1906, 0),
    JS_PROP_INT32_DEF("RGB", 0x1907, 0),
    JS_PROP_INT32_DEF("RGBA", 0x1908, 0),
    JS_PROP_INT32_DEF("LUMINANCE", 0x1909, 0),
    JS_PROP_INT32_DEF("LUMINANCE_ALPHA", 0x190A, 0),
    JS_PROP_INT32_DEF("DEPTH_COMPONENT", 0x1902, 0),
    JS_PROP_INT32_DEF("DEPTH_STENCIL", 0x84F9, 0),
    // clearBuffer* buffer selectors (COLOR doubles as a pixel-copy source).
    JS_PROP_INT32_DEF("COLOR", 0x1800, 0),
    JS_PROP_INT32_DEF("DEPTH", 0x1801, 0),
    JS_PROP_INT32_DEF("STENCIL", 0x1802, 0),
    JS_PROP_INT32_DEF("R8", 0x8229, 0),
    JS_PROP_INT32_DEF("R16F", 0x822D, 0),
    JS_PROP_INT32_DEF("R32F", 0x822E, 0),
    JS_PROP_INT32_DEF("RG8", 0x822B, 0),
    JS_PROP_INT32_DEF("RG16F", 0x822F, 0),
    JS_PROP_INT32_DEF("RG32F", 0x8230, 0),
    JS_PROP_INT32_DEF("RGB8", 0x8051, 0),
    JS_PROP_INT32_DEF("RGBA8", 0x8058, 0),
    JS_PROP_INT32_DEF("SRGB8", 0x8C41, 0),
    JS_PROP_INT32_DEF("SRGB8_ALPHA8", 0x8C43, 0),
    JS_PROP_INT32_DEF("RGB16F", 0x881B, 0),
    JS_PROP_INT32_DEF("RGB32F", 0x8815, 0),
    JS_PROP_INT32_DEF("RGBA16F", 0x881A, 0),
    JS_PROP_INT32_DEF("RGBA32F", 0x8814, 0),
    JS_PROP_INT32_DEF("R11F_G11F_B10F", 0x8C3A, 0),
    JS_PROP_INT32_DEF("RGB9_E5", 0x8C3D, 0),
    JS_PROP_INT32_DEF("DEPTH_COMPONENT16", 0x81A5, 0),
    JS_PROP_INT32_DEF("DEPTH_COMPONENT24", 0x81A6, 0),
    JS_PROP_INT32_DEF("DEPTH_COMPONENT32F", 0x8CAC, 0),
    JS_PROP_INT32_DEF("DEPTH24_STENCIL8", 0x88F0, 0),
    JS_PROP_INT32_DEF("DEPTH32F_STENCIL8", 0x8CAD, 0),
    JS_PROP_INT32_DEF("RED", 0x1903, 0),
    JS_PROP_INT32_DEF("RG", 0x8227, 0),
    JS_PROP_INT32_DEF("RED_INTEGER", 0x8D94, 0),
    JS_PROP_INT32_DEF("RG_INTEGER", 0x8228, 0),
    JS_PROP_INT32_DEF("RGB_INTEGER", 0x8D98, 0),
    JS_PROP_INT32_DEF("RGBA_INTEGER", 0x8D99, 0),
    JS_PROP_INT32_DEF("R8I", 0x8231, 0),
    JS_PROP_INT32_DEF("R8UI", 0x8232, 0),
    JS_PROP_INT32_DEF("R16I", 0x8233, 0),
    JS_PROP_INT32_DEF("R16UI", 0x8234, 0),
    JS_PROP_INT32_DEF("R32I", 0x8235, 0),
    JS_PROP_INT32_DEF("R32UI", 0x8236, 0),
    JS_PROP_INT32_DEF("RG8I", 0x8237, 0),
    JS_PROP_INT32_DEF("RG8UI", 0x8238, 0),
    JS_PROP_INT32_DEF("RG16I", 0x8239, 0),
    JS_PROP_INT32_DEF("RG16UI", 0x823A, 0),
    JS_PROP_INT32_DEF("RG32I", 0x823B, 0),
    JS_PROP_INT32_DEF("RG32UI", 0x823C, 0),
    JS_PROP_INT32_DEF("RGBA8I", 0x8D8E, 0),
    JS_PROP_INT32_DEF("RGBA8UI", 0x8D7C, 0),
    JS_PROP_INT32_DEF("RGBA16I", 0x8D88, 0),
    JS_PROP_INT32_DEF("RGBA16UI", 0x8D76, 0),
    JS_PROP_INT32_DEF("RGBA32I", 0x8D82, 0),
    JS_PROP_INT32_DEF("RGBA32UI", 0x8D70, 0),
    JS_PROP_INT32_DEF("RGB10_A2", 0x8059, 0),
    JS_PROP_INT32_DEF("RGB10_A2UI", 0x906F, 0),
    JS_PROP_INT32_DEF("UNSIGNED_INT_2_10_10_10_REV", 0x8368, 0),
    JS_PROP_INT32_DEF("UNSIGNED_SHORT_5_6_5", 0x8363, 0),
    JS_PROP_INT32_DEF("UNSIGNED_SHORT_4_4_4_4", 0x8033, 0),
    JS_PROP_INT32_DEF("UNSIGNED_SHORT_5_5_5_1", 0x8034, 0),
    JS_PROP_INT32_DEF("UNSIGNED_INT_24_8", 0x84FA, 0),
    JS_PROP_INT32_DEF("FLOAT_32_UNSIGNED_INT_24_8_REV", 0x8DAD, 0),

    // Shaders
    JS_PROP_INT32_DEF("VERTEX_SHADER", 0x8B31, 0),
    JS_PROP_INT32_DEF("FRAGMENT_SHADER", 0x8B30, 0),
    JS_PROP_INT32_DEF("COMPILE_STATUS", 0x8B81, 0),
    JS_PROP_INT32_DEF("LINK_STATUS", 0x8B82, 0),
    JS_PROP_INT32_DEF("DELETE_STATUS", 0x8B80, 0),
    JS_PROP_INT32_DEF("VALIDATE_STATUS", 0x8B83, 0),
    JS_PROP_INT32_DEF("ATTACHED_SHADERS", 0x8B85, 0),
    JS_PROP_INT32_DEF("ACTIVE_UNIFORMS", 0x8B86, 0),
    JS_PROP_INT32_DEF("ACTIVE_ATTRIBUTES", 0x8B89, 0),
    JS_PROP_INT32_DEF("ACTIVE_UNIFORM_BLOCKS", 0x8A36, 0),
    JS_PROP_INT32_DEF("SHADER_TYPE", 0x8B4F, 0),

    // Uniform / attribute types (getActiveUniform / getActiveAttrib)
    JS_PROP_INT32_DEF("FLOAT_VEC2", 0x8B50, 0),
    JS_PROP_INT32_DEF("FLOAT_VEC3", 0x8B51, 0),
    JS_PROP_INT32_DEF("FLOAT_VEC4", 0x8B52, 0),
    JS_PROP_INT32_DEF("INT_VEC2", 0x8B53, 0),
    JS_PROP_INT32_DEF("INT_VEC3", 0x8B54, 0),
    JS_PROP_INT32_DEF("INT_VEC4", 0x8B55, 0),
    JS_PROP_INT32_DEF("BOOL", 0x8B56, 0),
    JS_PROP_INT32_DEF("BOOL_VEC2", 0x8B57, 0),
    JS_PROP_INT32_DEF("BOOL_VEC3", 0x8B58, 0),
    JS_PROP_INT32_DEF("BOOL_VEC4", 0x8B59, 0),
    JS_PROP_INT32_DEF("FLOAT_MAT2", 0x8B5A, 0),
    JS_PROP_INT32_DEF("FLOAT_MAT3", 0x8B5B, 0),
    JS_PROP_INT32_DEF("FLOAT_MAT4", 0x8B5C, 0),
    JS_PROP_INT32_DEF("SAMPLER_2D", 0x8B5E, 0),
    JS_PROP_INT32_DEF("SAMPLER_3D", 0x8B5F, 0),
    JS_PROP_INT32_DEF("SAMPLER_CUBE", 0x8B60, 0),
    JS_PROP_INT32_DEF("SAMPLER_2D_ARRAY", 0x8DC1, 0),
    JS_PROP_INT32_DEF("UNSIGNED_INT_VEC2", 0x8DC6, 0),
    JS_PROP_INT32_DEF("UNSIGNED_INT_VEC3", 0x8DC7, 0),
    JS_PROP_INT32_DEF("UNSIGNED_INT_VEC4", 0x8DC8, 0),

    // Framebuffers
    JS_PROP_INT32_DEF("FRAMEBUFFER", 0x8D40, 0),
    JS_PROP_INT32_DEF("RENDERBUFFER", 0x8D41, 0),
    JS_PROP_INT32_DEF("COLOR_ATTACHMENT0", 0x8CE0, 0),
    JS_PROP_INT32_DEF("COLOR_ATTACHMENT1", 0x8CE1, 0),
    JS_PROP_INT32_DEF("COLOR_ATTACHMENT2", 0x8CE2, 0),
    JS_PROP_INT32_DEF("COLOR_ATTACHMENT3", 0x8CE3, 0),
    JS_PROP_INT32_DEF("DEPTH_ATTACHMENT", 0x8D00, 0),
    JS_PROP_INT32_DEF("STENCIL_ATTACHMENT", 0x8D20, 0),
    JS_PROP_INT32_DEF("DEPTH_STENCIL_ATTACHMENT", 0x821A, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_COMPLETE", 0x8CD5, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_INCOMPLETE_ATTACHMENT", 0x8CD6, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", 0x8CD7, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_INCOMPLETE_DIMENSIONS", 0x8CD9, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_UNSUPPORTED", 0x8CDD, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_INCOMPLETE_MULTISAMPLE", 0x8D56, 0),
    JS_PROP_INT32_DEF("READ_FRAMEBUFFER", 0x8CA8, 0),
    JS_PROP_INT32_DEF("DRAW_FRAMEBUFFER", 0x8CA9, 0),
    JS_PROP_INT32_DEF("DRAW_BUFFER0", 0x8825, 0),
    JS_PROP_INT32_DEF("NONE", 0, 0),

    // Pixel store
    JS_PROP_INT32_DEF("UNPACK_ALIGNMENT", 0x0CF5, 0),
    JS_PROP_INT32_DEF("PACK_ALIGNMENT", 0x0D05, 0),
    JS_PROP_INT32_DEF("UNPACK_FLIP_Y_WEBGL", 0x9240, 0),
    JS_PROP_INT32_DEF("UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241, 0),
    JS_PROP_INT32_DEF("UNPACK_COLORSPACE_CONVERSION_WEBGL", 0x9243, 0),
    JS_PROP_INT32_DEF("UNPACK_ROW_LENGTH", 0x0CF2, 0),
    JS_PROP_INT32_DEF("UNPACK_IMAGE_HEIGHT", 0x806E, 0),
    JS_PROP_INT32_DEF("UNPACK_SKIP_PIXELS", 0x0CF4, 0),
    JS_PROP_INT32_DEF("UNPACK_SKIP_ROWS", 0x0CF3, 0),
    JS_PROP_INT32_DEF("UNPACK_SKIP_IMAGES", 0x806D, 0),

    // getParameter — state queries
    JS_PROP_INT32_DEF("VIEWPORT", 0x0BA2, 0),
    JS_PROP_INT32_DEF("SCISSOR_BOX", 0x0C10, 0),
    JS_PROP_INT32_DEF("COLOR_CLEAR_VALUE", 0x0C22, 0),
    JS_PROP_INT32_DEF("COLOR_WRITEMASK", 0x0C23, 0),
    JS_PROP_INT32_DEF("DEPTH_CLEAR_VALUE", 0x0B73, 0),
    JS_PROP_INT32_DEF("DEPTH_WRITEMASK", 0x0B72, 0),
    JS_PROP_INT32_DEF("DEPTH_RANGE", 0x0B70, 0),
    JS_PROP_INT32_DEF("DEPTH_FUNC", 0x0B74, 0),
    JS_PROP_INT32_DEF("STENCIL_CLEAR_VALUE", 0x0B91, 0),
    JS_PROP_INT32_DEF("STENCIL_FUNC", 0x0B92, 0),
    JS_PROP_INT32_DEF("STENCIL_REF", 0x0B97, 0),
    JS_PROP_INT32_DEF("STENCIL_VALUE_MASK", 0x0B93, 0),
    JS_PROP_INT32_DEF("STENCIL_WRITEMASK", 0x0B98, 0),
    JS_PROP_INT32_DEF("BLEND_SRC_RGB", 0x80C9, 0),
    JS_PROP_INT32_DEF("BLEND_DST_RGB", 0x80C8, 0),
    JS_PROP_INT32_DEF("BLEND_SRC_ALPHA", 0x80CB, 0),
    JS_PROP_INT32_DEF("BLEND_DST_ALPHA", 0x80CA, 0),
    JS_PROP_INT32_DEF("BLEND_EQUATION_RGB", 0x8009, 0),
    JS_PROP_INT32_DEF("BLEND_EQUATION_ALPHA", 0x883D, 0),
    JS_PROP_INT32_DEF("BLEND_COLOR", 0x8005, 0),
    JS_PROP_INT32_DEF("CULL_FACE_MODE", 0x0B45, 0),
    JS_PROP_INT32_DEF("FRONT_FACE", 0x0B46, 0),
    JS_PROP_INT32_DEF("LINE_WIDTH", 0x0B21, 0),
    JS_PROP_INT32_DEF("ACTIVE_TEXTURE", 0x84E0, 0),
    JS_PROP_INT32_DEF("ALIASED_POINT_SIZE_RANGE", 0x846D, 0),
    JS_PROP_INT32_DEF("ALIASED_LINE_WIDTH_RANGE", 0x846E, 0),
    JS_PROP_INT32_DEF("SAMPLE_BUFFERS", 0x80A8, 0),
    JS_PROP_INT32_DEF("SAMPLES", 0x80A9, 0),
    JS_PROP_INT32_DEF("SUBPIXEL_BITS", 0x0D50, 0),
    JS_PROP_INT32_DEF("IMPLEMENTATION_COLOR_READ_TYPE", 0x8B9A, 0),
    JS_PROP_INT32_DEF("IMPLEMENTATION_COLOR_READ_FORMAT", 0x8B9B, 0),
    JS_PROP_INT32_DEF("ARRAY_BUFFER_BINDING", 0x8894, 0),
    JS_PROP_INT32_DEF("ELEMENT_ARRAY_BUFFER_BINDING", 0x8895, 0),
    JS_PROP_INT32_DEF("UNIFORM_BUFFER_BINDING", 0x8A28, 0),
    JS_PROP_INT32_DEF("CURRENT_PROGRAM", 0x8B8D, 0),
    JS_PROP_INT32_DEF("FRAMEBUFFER_BINDING", 0x8CA6, 0),
    JS_PROP_INT32_DEF("RENDERBUFFER_BINDING", 0x8CA7, 0),
    JS_PROP_INT32_DEF("TEXTURE_BINDING_2D", 0x8069, 0),
    JS_PROP_INT32_DEF("TEXTURE_BINDING_CUBE_MAP", 0x8514, 0),
    JS_PROP_INT32_DEF("TEXTURE_BINDING_3D", 0x806A, 0),
    JS_PROP_INT32_DEF("TEXTURE_BINDING_2D_ARRAY", 0x8C1D, 0),
    JS_PROP_INT32_DEF("VERTEX_ARRAY_BINDING", 0x85B5, 0),
    JS_PROP_INT32_DEF("UNIFORM_BUFFER_OFFSET_ALIGNMENT", 0x8A34, 0),
    JS_PROP_INT32_DEF("TEXTURE_IMMUTABLE_FORMAT", 0x912F, 0),
    JS_PROP_INT32_DEF("BROWSER_DEFAULT_WEBGL", 0x9244, 0),
    JS_PROP_INT32_DEF("COMPRESSED_TEXTURE_FORMATS", 0x86A3, 0),

    // getParameter — implementation limits
    JS_PROP_INT32_DEF("MAX_TEXTURE_SIZE", 0x0D33, 0),
    JS_PROP_INT32_DEF("MAX_CUBE_MAP_TEXTURE_SIZE", 0x851C, 0),
    JS_PROP_INT32_DEF("MAX_RENDERBUFFER_SIZE", 0x84E8, 0),
    JS_PROP_INT32_DEF("MAX_VIEWPORT_DIMS", 0x0D3A, 0),
    JS_PROP_INT32_DEF("MAX_VERTEX_ATTRIBS", 0x8869, 0),
    JS_PROP_INT32_DEF("MAX_VERTEX_UNIFORM_VECTORS", 0x8DFB, 0),
    JS_PROP_INT32_DEF("MAX_FRAGMENT_UNIFORM_VECTORS", 0x8DFD, 0),
    JS_PROP_INT32_DEF("MAX_VARYING_VECTORS", 0x8DFC, 0),
    JS_PROP_INT32_DEF("MAX_COMBINED_TEXTURE_IMAGE_UNITS", 0x8B4D, 0),
    JS_PROP_INT32_DEF("MAX_VERTEX_TEXTURE_IMAGE_UNITS", 0x8B4C, 0),
    JS_PROP_INT32_DEF("MAX_TEXTURE_IMAGE_UNITS", 0x8872, 0),
    JS_PROP_INT32_DEF("MAX_DRAW_BUFFERS", 0x8824, 0),
    JS_PROP_INT32_DEF("MAX_COLOR_ATTACHMENTS", 0x8CDF, 0),
    JS_PROP_INT32_DEF("MAX_SAMPLES", 0x8D57, 0),
    JS_PROP_INT32_DEF("MAX_UNIFORM_BUFFER_BINDINGS", 0x8A2F, 0),
    JS_PROP_INT32_DEF("MAX_UNIFORM_BLOCK_SIZE", 0x8A30, 0),
    JS_PROP_INT32_DEF("MAX_VERTEX_UNIFORM_BLOCKS", 0x8A2B, 0),
    JS_PROP_INT32_DEF("MAX_FRAGMENT_UNIFORM_BLOCKS", 0x8A2D, 0),
    JS_PROP_INT32_DEF("MAX_ELEMENTS_VERTICES", 0x80E8, 0),
    JS_PROP_INT32_DEF("MAX_ELEMENTS_INDICES", 0x80E9, 0),
    JS_PROP_INT32_DEF("MAX_3D_TEXTURE_SIZE", 0x8073, 0),
    JS_PROP_INT32_DEF("MAX_ARRAY_TEXTURE_LAYERS", 0x88FF, 0),

    // String queries
    JS_PROP_INT32_DEF("VERSION", 0x1F02, 0),
    JS_PROP_INT32_DEF("SHADING_LANGUAGE_VERSION", 0x8B8C, 0),
    JS_PROP_INT32_DEF("VENDOR", 0x1F00, 0),
    JS_PROP_INT32_DEF("RENDERER", 0x1F01, 0),

    // Shader precision
    JS_PROP_INT32_DEF("LOW_FLOAT", 0x8DF0, 0),
    JS_PROP_INT32_DEF("MEDIUM_FLOAT", 0x8DF1, 0),
    JS_PROP_INT32_DEF("HIGH_FLOAT", 0x8DF2, 0),
    JS_PROP_INT32_DEF("LOW_INT", 0x8DF3, 0),
    JS_PROP_INT32_DEF("MEDIUM_INT", 0x8DF4, 0),
    JS_PROP_INT32_DEF("HIGH_INT", 0x8DF5, 0),

    // Misc
    JS_PROP_INT32_DEF("NO_ERROR", 0, 0),
    JS_PROP_INT32_DEF("INVALID_ENUM", 0x0500, 0),
    JS_PROP_INT32_DEF("INVALID_VALUE", 0x0501, 0),
    JS_PROP_INT32_DEF("INVALID_OPERATION", 0x0502, 0),
    JS_PROP_INT32_DEF("OUT_OF_MEMORY", 0x0505, 0),
    JS_PROP_INT32_DEF("INVALID_FRAMEBUFFER_OPERATION", 0x0506, 0),
    JS_PROP_INT32_DEF("CONTEXT_LOST_WEBGL", 0x9242, 0),

    // Sampler objects
    JS_PROP_INT32_DEF("SAMPLER_BINDING", 0x8919, 0),

    // Sync objects
    JS_PROP_INT32_DEF("OBJECT_TYPE", 0x9112, 0),
    JS_PROP_INT32_DEF("SYNC_CONDITION", 0x9113, 0),
    JS_PROP_INT32_DEF("SYNC_STATUS", 0x9114, 0),
    JS_PROP_INT32_DEF("SYNC_FLAGS", 0x9115, 0),
    JS_PROP_INT32_DEF("SYNC_FENCE", 0x9116, 0),
    JS_PROP_INT32_DEF("SYNC_GPU_COMMANDS_COMPLETE", 0x9117, 0),
    JS_PROP_INT32_DEF("UNSIGNALED", 0x9118, 0),
    JS_PROP_INT32_DEF("SIGNALED", 0x9119, 0),
    JS_PROP_INT32_DEF("ALREADY_SIGNALED", 0x911A, 0),
    JS_PROP_INT32_DEF("TIMEOUT_EXPIRED", 0x911B, 0),
    JS_PROP_INT32_DEF("CONDITION_SATISFIED", 0x911C, 0),
    JS_PROP_INT32_DEF("WAIT_FAILED", 0x911D, 0),
    JS_PROP_INT32_DEF("SYNC_FLUSH_COMMANDS_BIT", 0x00000001, 0),
    JS_PROP_INT32_DEF("MAX_CLIENT_WAIT_TIMEOUT_WEBGL", 0x9247, 0),
    JS_PROP_DOUBLE_DEF("TIMEOUT_IGNORED", -1.0, 0),

    // Transform feedback
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK", 0x8E22, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_BINDING", 0x8E25, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_BUFFER_BINDING", 0x8C8F, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_BUFFER_START", 0x8C84, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_BUFFER_SIZE", 0x8C85, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_BUFFER_MODE", 0x8C7F, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_VARYINGS", 0x8C83, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_ACTIVE", 0x8E24, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_PAUSED", 0x8E23, 0),
    JS_PROP_INT32_DEF("INTERLEAVED_ATTRIBS", 0x8C8C, 0),
    JS_PROP_INT32_DEF("SEPARATE_ATTRIBS", 0x8C8D, 0),
    JS_PROP_INT32_DEF("MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS", 0x8C8A, 0),
    JS_PROP_INT32_DEF("MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS", 0x8C8B, 0),
    JS_PROP_INT32_DEF("MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS", 0x8C80, 0),

    // Indexed uniform-buffer ranges (getIndexedParameter)
    JS_PROP_INT32_DEF("UNIFORM_BUFFER_START", 0x8A29, 0),
    JS_PROP_INT32_DEF("UNIFORM_BUFFER_SIZE", 0x8A2A, 0),

    // Query objects
    JS_PROP_INT32_DEF("CURRENT_QUERY", 0x8865, 0),
    JS_PROP_INT32_DEF("QUERY_RESULT", 0x8866, 0),
    JS_PROP_INT32_DEF("QUERY_RESULT_AVAILABLE", 0x8867, 0),
    JS_PROP_INT32_DEF("ANY_SAMPLES_PASSED", 0x8C2F, 0),
    JS_PROP_INT32_DEF("ANY_SAMPLES_PASSED_CONSERVATIVE", 0x8D6A, 0),
    JS_PROP_INT32_DEF("TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN", 0x8C88, 0),

    // Uniform introspection (getActiveUniforms pnames)
    JS_PROP_INT32_DEF("UNIFORM_TYPE", 0x8A37, 0),
    JS_PROP_INT32_DEF("UNIFORM_SIZE", 0x8A38, 0),
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_INDEX", 0x8A3A, 0),
    JS_PROP_INT32_DEF("UNIFORM_OFFSET", 0x8A3B, 0),
    JS_PROP_INT32_DEF("UNIFORM_ARRAY_STRIDE", 0x8A3C, 0),
    JS_PROP_INT32_DEF("UNIFORM_MATRIX_STRIDE", 0x8A3D, 0),
    JS_PROP_INT32_DEF("UNIFORM_IS_ROW_MAJOR", 0x8A3E, 0),
    // Uniform block introspection (getActiveUniformBlockParameter pnames)
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_BINDING", 0x8A3F, 0),
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_ACTIVE_UNIFORMS", 0x8A42, 0),
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES", 0x8A43, 0),
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER", 0x8A44, 0),
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER", 0x8A46, 0),
    JS_PROP_INT32_DEF("UNIFORM_BLOCK_DATA_SIZE", 0x8A40, 0),
    // getUniformBlockIndex returns this as an unsigned 4294967295 — the
    // constant must match (an int32 -1 would never compare equal in JS).
    JS_PROP_DOUBLE_DEF("INVALID_INDEX", 4294967295.0, 0),
};
static const int webgl2_constants_count = sizeof(webgl2_constants) / sizeof(webgl2_constants[0]);

// ===========================================================================
// Install / wrap / cleanup
// ===========================================================================

// Tag type for WebGL2RenderingContext (borrowed pointer, no destructor)
struct WebGL2CtxTag {};


void WebGL2Bindings::install(JSContext* ctx)
{
    // --- Register WebGL2 context class (no destructor — C++ context owns GL lifetime) ---
    qjsbind::Class<WebGL2CtxTag>(ctx, "WebGL2RenderingContext",
                                  qjsbind::NoGlobal | qjsbind::NoDestructor)
        .function_list(webgl2_constants, webgl2_constants_count)
        .function_list(webgl2_state_funcs, webgl2_state_funcs_count)
        .function_list(webgl2_buffer_funcs, webgl2_buffer_funcs_count)
        .function_list(webgl2_shader_funcs, webgl2_shader_funcs_count)
        .function_list(webgl2_texture_funcs, webgl2_texture_funcs_count)
        .function_list(webgl2_framebuffer_funcs, webgl2_framebuffer_funcs_count)
        .function_list(webgl2_query_funcs, webgl2_query_funcs_count)
        .function_list(webgl2_object_funcs, webgl2_object_funcs_count);
    js_webgl2_ctx_class_id = qjsbind::class_id<WebGL2CtxTag>();

    // --- Register WebGL object classes (default delete-ptr finalizer) ---
    qjsbind::Class<bro::webgl::WebGLBuffer>(ctx, "WebGLBuffer", qjsbind::NoGlobal);
    js_webgl_buffer_class_id = qjsbind::class_id<bro::webgl::WebGLBuffer>();

    qjsbind::Class<bro::webgl::WebGLTexture>(ctx, "WebGLTexture", qjsbind::NoGlobal);
    js_webgl_texture_class_id = qjsbind::class_id<bro::webgl::WebGLTexture>();

    qjsbind::Class<bro::webgl::WebGLProgram>(ctx, "WebGLProgram", qjsbind::NoGlobal);
    js_webgl_program_class_id = qjsbind::class_id<bro::webgl::WebGLProgram>();

    qjsbind::Class<bro::webgl::WebGLShader>(ctx, "WebGLShader", qjsbind::NoGlobal);
    js_webgl_shader_class_id = qjsbind::class_id<bro::webgl::WebGLShader>();

    qjsbind::Class<bro::webgl::WebGLFramebuffer>(ctx, "WebGLFramebuffer", qjsbind::NoGlobal);
    js_webgl_framebuffer_class_id = qjsbind::class_id<bro::webgl::WebGLFramebuffer>();

    qjsbind::Class<bro::webgl::WebGLRenderbuffer>(ctx, "WebGLRenderbuffer", qjsbind::NoGlobal);
    js_webgl_renderbuffer_class_id = qjsbind::class_id<bro::webgl::WebGLRenderbuffer>();

    qjsbind::Class<bro::webgl::WebGLVertexArrayObject>(ctx, "WebGLVertexArrayObject", qjsbind::NoGlobal);
    js_webgl_vao_class_id = qjsbind::class_id<bro::webgl::WebGLVertexArrayObject>();

    qjsbind::Class<bro::webgl::WebGLUniformLocation>(ctx, "WebGLUniformLocation", qjsbind::NoGlobal);
    js_webgl_uniform_loc_class_id = qjsbind::class_id<bro::webgl::WebGLUniformLocation>();

    qjsbind::Class<bro::webgl::WebGLSampler>(ctx, "WebGLSampler", qjsbind::NoGlobal);
    js_webgl_sampler_class_id = qjsbind::class_id<bro::webgl::WebGLSampler>();

    qjsbind::Class<bro::webgl::WebGLQuery>(ctx, "WebGLQuery", qjsbind::NoGlobal);
    js_webgl_query_class_id = qjsbind::class_id<bro::webgl::WebGLQuery>();

    qjsbind::Class<bro::webgl::WebGLSync>(ctx, "WebGLSync", qjsbind::NoGlobal);
    js_webgl_sync_class_id = qjsbind::class_id<bro::webgl::WebGLSync>();

    qjsbind::Class<bro::webgl::WebGLTransformFeedback>(ctx, "WebGLTransformFeedback", qjsbind::NoGlobal);
    js_webgl_tf_class_id = qjsbind::class_id<bro::webgl::WebGLTransformFeedback>();

    // --- three.js compatibility: expose WebGL2RenderingContext as a global constructor ---
    // three.js checks: typeof WebGL2RenderingContext !== "undefined"
    // three.js checks: ctx.constructor.name === "WebGL2RenderingContext"
    JSValue proto = JS_GetClassProto(ctx, js_webgl2_ctx_class_id);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_NewObject(ctx);
    JSAtom nameAtom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyValue(ctx, ctor, nameAtom, JS_NewString(ctx, "WebGL2RenderingContext"),
                           JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, nameAtom);

    JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));
    JS_SetPropertyStr(ctx, global, "WebGL2RenderingContext", ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);

    // --- bro.image.gpu — WebGL2-backed colormap helpers. Extends bro.image
    //     (installed by brokit) with `.gpu`. Lives here because it depends on
    //     the WebGL2 API just registered above; only available in GPU mode. ---
    JSValue r = JS_Eval(ctx, js_image_gpu, std::strlen(js_image_gpu),
                        "<bro.image.gpu>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        if (msg) {
            JS_ThrowInternalError(ctx, "bro.image.gpu install failed: %s", msg);
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
}


JSValue WebGL2Bindings::wrapContext(JSContext* ctx, webgl::WebGL2RenderingContext* glCtx) {
    JSValue obj = qjsbind::wrap_unowned<WebGL2CtxTag>(ctx, reinterpret_cast<WebGL2CtxTag*>(glCtx));
    if (JS_IsException(obj)) return obj;

    // Three.js state.reset() accesses gl.canvas.width / gl.canvas.height
    JSValue canvas = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, canvas, "width", JS_NewInt32(ctx, glCtx->canvasWidth()));
    JS_SetPropertyStr(ctx, canvas, "height", JS_NewInt32(ctx, glCtx->canvasHeight()));
    JS_SetPropertyStr(ctx, obj, "canvas", canvas);

    return obj;
}

void WebGL2Bindings::cleanup(JSContext*) {
    // No persistent JSValue/atom storage in this binding — qjsbind owns the
    // class registration and prototype, freed when the JSRuntime is destroyed.
}



} // namespace bro::js

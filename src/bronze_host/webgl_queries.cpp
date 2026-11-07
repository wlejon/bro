// getParameter / getExtension and friends — query dispatch implementations
// for WebGL2 pnames and extensions. An unknown pname falls to the generic
// int path; an unknown extension returns null.
//
// What the client holds answers without the backend: the device's
// constants (caps), the extensions enabled, every sync call, and a query's
// status and result once published (webgl_client_async.h). Every other
// state query is a round trip whose callback answers plain data
// (GLValue, DynamicParameter), wrapped into a Value after it returns.
//
// Array-shaped pnames answer the typed array the WebGL IDL names
// (Int32Array for VIEWPORT, Float32Array for DEPTH_RANGE, Uint32Array for
// COMPRESSED_TEXTURE_FORMATS, ...), and sequence<GLboolean> ones a real Array.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_values.h"
#include "webgl/vulkan/webgl_vk_caps.h"

#include <cstring>
#include <string>

namespace bro::bronze_host {

namespace {

// A GLValue as the IDL types it: a number or boolean, a buffer object, or a
// Float32Array / Int32Array / Uint32Array / sequence<boolean>.
Value glValue(webgl::WebGL2RenderingContext* c, const webgl::GLValue& v) {
    using Kind = webgl::GLValue::Kind;
    if (v.kind == Kind::Buffer) return v.words[0] ? webglObject(c, WebGLCell::Buffer, v.words[0]) : ev::null();
    if (!v.array) {
        switch (v.kind) {
            case Kind::Float: { float f; std::memcpy(&f, &v.words[0], 4); return ev::fromDouble(f); }
            case Kind::Int: return ev::fromDouble(static_cast<int32_t>(v.words[0]));
            case Kind::Bool: return ev::fromBool(v.words[0] != 0);
            default: return ev::fromDouble(v.words[0]);
        }
    }
    switch (v.kind) {
        case Kind::Float: {
            float f[16];
            std::memcpy(f, v.words, v.count * 4);
            return makeFloat32Array(f, v.count);
        }
        case Kind::Int: {
            int32_t i[16];
            std::memcpy(i, v.words, v.count * 4);
            return makeInt32Array(i, v.count);
        }
        case Kind::Bool: return hostArrayOf(v.count, [&v](size_t i) { return ev::fromBool(v.words[i] != 0); });
        default: return makeUint32Array(v.words, v.count);
    }
}

// An extension object: its constants, and methods for the few that have them.
Value makeExtension(webgl::WebGL2RenderingContext* c, const std::string& name) {
    ObjectBuilder o;
    auto def = [&o](const char* n, double v) { o.set(n, ev::fromDouble(v)); };
    if (name == "WEBGL_compressed_texture_s3tc") {
        def("COMPRESSED_RGB_S3TC_DXT1_EXT", 0x83F0);
        def("COMPRESSED_RGBA_S3TC_DXT1_EXT", 0x83F1);
        def("COMPRESSED_RGBA_S3TC_DXT3_EXT", 0x83F2);
        def("COMPRESSED_RGBA_S3TC_DXT5_EXT", 0x83F3);
    } else if (name == "WEBGL_compressed_texture_s3tc_srgb") {
        def("COMPRESSED_SRGB_S3TC_DXT1_EXT", 0x8C4C);
        def("COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT", 0x8C4D);
        def("COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT", 0x8C4E);
        def("COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT", 0x8C4F);
    } else if (name == "EXT_texture_compression_rgtc") {
        def("COMPRESSED_RED_RGTC1_EXT", 0x8DBB);
        def("COMPRESSED_SIGNED_RED_RGTC1_EXT", 0x8DBC);
        def("COMPRESSED_RED_GREEN_RGTC2_EXT", 0x8DBD);
        def("COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT", 0x8DBE);
    } else if (name == "EXT_texture_compression_bptc") {
        def("COMPRESSED_RGBA_BPTC_UNORM_EXT", 0x8E8C);
        def("COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT", 0x8E8D);
        def("COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT", 0x8E8E);
        def("COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT", 0x8E8F);
    } else if (name == "WEBGL_compressed_texture_etc") {
        static constexpr const char* kNames[] = {
            "COMPRESSED_R11_EAC", "COMPRESSED_SIGNED_R11_EAC", "COMPRESSED_RG11_EAC",
            "COMPRESSED_SIGNED_RG11_EAC", "COMPRESSED_RGB8_ETC2", "COMPRESSED_SRGB8_ETC2",
            "COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2", "COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2",
            "COMPRESSED_RGBA8_ETC2_EAC", "COMPRESSED_SRGB8_ALPHA8_ETC2_EAC"};
        for (size_t i = 0; i < std::size(kNames); ++i) def(kNames[i], 0x9270 + static_cast<double>(i));
    } else if (name == "WEBGL_compressed_texture_etc1") {
        def("COMPRESSED_RGB_ETC1_WEBGL", 0x8D64);
    } else if (name == "WEBGL_compressed_texture_astc") {
        // Block sizes in enum order: RGBA from 0x93B0, SRGB8_ALPHA8 from 0x93D0.
        static constexpr const char* kBlocks[] = {"4x4", "5x4", "5x5", "6x5", "6x6", "8x5", "8x6",
                                                  "8x8", "10x5", "10x6", "10x8", "10x10", "12x10", "12x12"};
        for (size_t i = 0; i < std::size(kBlocks); ++i) {
            def(("COMPRESSED_RGBA_ASTC_" + std::string(kBlocks[i]) + "_KHR").c_str(),
                0x93B0 + static_cast<double>(i));
            def(("COMPRESSED_SRGB8_ALPHA8_ASTC_" + std::string(kBlocks[i]) + "_KHR").c_str(),
                0x93D0 + static_cast<double>(i));
        }
        // Only the LDR profile: no HDR, no 3D blocks.
        o.def("getSupportedProfiles", 0, [](Value, std::span<const Value>) {
            return hostArrayOf(1, [](size_t) { return ev::fromUtf8("ldr"); });
        });
    } else if (name == "EXT_texture_filter_anisotropic") {
        def("TEXTURE_MAX_ANISOTROPY_EXT", 0x84FE);
        def("MAX_TEXTURE_MAX_ANISOTROPY_EXT", 0x84FF);
    } else if (name == "EXT_disjoint_timer_query_webgl2") {
        def("QUERY_COUNTER_BITS_EXT", 0x8864);
        def("TIME_ELAPSED_EXT", 0x88BF);
        def("TIMESTAMP_EXT", 0x8E28);
        def("GPU_DISJOINT_EXT", 0x8FBB);
        o.def("queryCounterEXT", 2, [c](Value, std::span<const Value> a) {
            if (auto* cl = client(c)) cl->queryCounter(idOf(argAt(a, 0), WebGLCell::Query), u32At(a, 1));
            return ev::undefined();
        });
    } else if (name == "KHR_parallel_shader_compile") {
        def("MAX_SHADER_COMPILER_THREADS_KHR", 0x91B0);
        def("COMPLETION_STATUS_KHR", 0x91B1);
        o.def("maxShaderCompilerThreadsKHR", 1, [c](Value, std::span<const Value> a) {
            if (auto* cl = client(c)) cl->maxShaderCompilerThreads(u32At(a, 0));
            return ev::undefined();
        });
    } else if (name == "EXT_color_buffer_half_float") {
        def("RGBA16F_EXT", 0x881A);
        def("RGB16F_EXT", 0x881B);
        def("FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", 0x8211);
        def("UNSIGNED_NORMALIZED_EXT", 0x8C17);
    } else if (name == "WEBGL_debug_renderer_info") {
        def("UNMASKED_VENDOR_WEBGL", 0x9245);
        def("UNMASKED_RENDERER_WEBGL", 0x9246);
    } else if (name == "BRO_buffer_map") {
        def("MAP_READ_BIT", 0x0001);
        def("MAP_WRITE_BIT", 0x0002);
        def("MAP_INVALIDATE_RANGE_BIT", 0x0004);
        def("MAP_INVALIDATE_BUFFER_BIT", 0x0008);
        def("MAP_FLUSH_EXPLICIT_BIT", 0x0010);
        def("MAP_UNSYNCHRONIZED_BIT", 0x0020);
    } else if (name == "WEBGL_lose_context") {
        o.def("loseContext", 0, [c](Value, std::span<const Value>) {
            detachWebGLMappings(c);
            c->loseContext();
            forgetWebGLObjects(c);
            return ev::undefined();
        });
        o.def("restoreContext", 0, [c](Value, std::span<const Value>) {
            c->restoreContext();
            return ev::undefined();
        });
    }
    // An empty object for a supported extension with nothing to expose.
    return o.get();
}

// The buffer target each *_BUFFER_BINDING pname reports.
GLenum bufferBindingTarget(GLenum pname) {
    switch (pname) {
        case 0x8894: return GL_ARRAY_BUFFER;               // ARRAY_BUFFER_BINDING
        case 0x8895: return GL_ELEMENT_ARRAY_BUFFER;       // ELEMENT_ARRAY_BUFFER_BINDING
        case 0x88ED: return GL_PIXEL_PACK_BUFFER;          // PIXEL_PACK_BUFFER_BINDING
        case 0x88EF: return GL_PIXEL_UNPACK_BUFFER;        // PIXEL_UNPACK_BUFFER_BINDING
        case 0x8A28: return GL_UNIFORM_BUFFER;             // UNIFORM_BUFFER_BINDING
        case 0x8C8F: return GL_TRANSFORM_FEEDBACK_BUFFER;  // TRANSFORM_FEEDBACK_BUFFER_BINDING
        case 0x8F36: return GL_COPY_READ_BUFFER;           // COPY_READ_BUFFER_BINDING
        case 0x8F37: return GL_COPY_WRITE_BUFFER;          // COPY_WRITE_BUFFER_BINDING
        default: return 0;
    }
}

// The texture target each TEXTURE_BINDING_* pname reports.
GLenum textureBindingTarget(GLenum pname) {
    switch (pname) {
        case 0x8069: return GL_TEXTURE_2D;        // TEXTURE_BINDING_2D
        case 0x8514: return GL_TEXTURE_CUBE_MAP;  // TEXTURE_BINDING_CUBE_MAP
        case 0x806A: return GL_TEXTURE_3D;        // TEXTURE_BINDING_3D
        case 0x8C1D: return GL_TEXTURE_2D_ARRAY;  // TEXTURE_BINDING_2D_ARRAY
        default: return 0;
    }
}

// A getParameter answer the backend's state makes: plain data, made inside
// a round trip and wrapped into a Value after it.
struct DynamicParameter {
    enum class Kind : uint8_t { Number, Bool, Int32Array, Float32Array, BoolArray, Object } kind = Kind::Number;
    double number = 0;
    uint32_t count = 0;
    int32_t ints[4]{};
    float floats[4]{};
    bool bools[4]{};
    uint32_t objectKind = 0;  // WebGLCell kind; objectId 0 (or gone) answers null
    GLuint objectId = 0;
};

DynamicParameter number(double v) {
    DynamicParameter d;
    d.number = v;
    return d;
}

DynamicParameter boolean(bool v) {
    DynamicParameter d;
    d.kind = DynamicParameter::Kind::Bool;
    d.bools[0] = v;
    return d;
}

// The object an *_BINDING query names: none for none, or for one no longer
// live (deleted, or never bound).
DynamicParameter binding(uint32_t kind, GLuint id, bool exists) {
    DynamicParameter d;
    d.kind = DynamicParameter::Kind::Object;
    d.objectKind = kind;
    d.objectId = id != 0 && exists ? id : 0;
    return d;
}

// getParameter of a pname the backend's state answers. Runs in a round
// trip: no Value made here.
DynamicParameter dynamicParameter(WebGLBackend& gl, GLenum pname) {
    using Kind = DynamicParameter::Kind;
    DynamicParameter d;
    switch (pname) {
        // Float parameters
        case 0x0B73:  // GL_DEPTH_CLEAR_VALUE
        case 0x0B21:  // GL_LINE_WIDTH
        case 0x80AA:  // GL_SAMPLE_COVERAGE_VALUE
        case 0x8038:  // GL_POLYGON_OFFSET_FACTOR
        case 0x2A00:  // GL_POLYGON_OFFSET_UNITS
        case 0x84FF:  // MAX_TEXTURE_MAX_ANISOTROPY_EXT (INVALID_ENUM: not in caps, so not supported)
            return number(gl.getParameterFloat(pname));
        case 0x8E28:  // TIMESTAMP_EXT
            return number(static_cast<double>(gl.gpuTimestamp()));

        // Int[4]
        case 0x0BA2:  // GL_VIEWPORT
        case 0x0C10:  // GL_SCISSOR_BOX
            d.kind = Kind::Int32Array;
            d.count = 4;
            gl.getParameterInt4(pname, d.ints);
            return d;
        case 0x0B70:  // GL_DEPTH_RANGE
            d.kind = Kind::Float32Array;
            d.count = 2;
            gl.getParameterFloat2(pname, d.floats);
            return d;
        // Float[4]
        case 0x0C22:  // GL_COLOR_CLEAR_VALUE
        case 0x8005:  // GL_BLEND_COLOR
            d.kind = Kind::Float32Array;
            d.count = 4;
            gl.getParameterFloat4(pname, d.floats);
            return d;
        case 0x0C23: {  // GL_COLOR_WRITEMASK
            GLboolean v[4] = {0, 0, 0, 0};
            gl.getParameterBool4(pname, v);
            d.kind = Kind::BoolArray;
            d.count = 4;
            for (int i = 0; i < 4; ++i) d.bools[i] = v[i] != GL_FALSE;
            return d;
        }

        // WebGL-only pixel-store state.
        case 0x9240:  // UNPACK_FLIP_Y_WEBGL
        case 0x9241:  // UNPACK_PREMULTIPLY_ALPHA_WEBGL
            return boolean(gl.getPixelStorei(pname) != 0);
        case 0x9243:  // UNPACK_COLORSPACE_CONVERSION_WEBGL
            return number(gl.getPixelStorei(pname));

        // Object-binding queries.
        case 0x8894: case 0x8895: case 0x88ED: case 0x88EF:
        case 0x8A28: case 0x8C8F: case 0x8F36: case 0x8F37: {  // *_BUFFER_BINDING
            const GLuint id = gl.boundBuffer(bufferBindingTarget(pname));
            return binding(WebGLCell::Buffer, id, gl.isBuffer({id}));
        }
        case 0x8069: case 0x8514: case 0x806A: case 0x8C1D: {  // TEXTURE_BINDING_*
            const GLuint id = gl.boundTexture(textureBindingTarget(pname));
            return binding(WebGLCell::Texture, id, gl.isTexture({id}));
        }
        case 0x8B8D: {  // CURRENT_PROGRAM
            const GLuint id = gl.currentProgram();
            return binding(WebGLCell::Program, id, gl.isProgram({id}));
        }
        case 0x8CA6: {  // FRAMEBUFFER_BINDING / DRAW_FRAMEBUFFER_BINDING
            const GLuint id = gl.drawFramebufferBinding();
            return binding(WebGLCell::Framebuffer, id, gl.isFramebuffer({id}));
        }
        case 0x8CAA: {  // READ_FRAMEBUFFER_BINDING
            const GLuint id = gl.readFramebufferBinding();
            return binding(WebGLCell::Framebuffer, id, gl.isFramebuffer({id}));
        }
        case 0x8CA7: {  // RENDERBUFFER_BINDING
            const GLuint id = gl.renderbufferBinding();
            return binding(WebGLCell::Renderbuffer, id, gl.isRenderbuffer({id}));
        }
        case 0x85B5: {  // VERTEX_ARRAY_BINDING
            const GLuint id = gl.currentVertexArray();
            return binding(WebGLCell::VertexArray, id, gl.isVertexArray({id}));
        }
        case 0x8919: {  // SAMPLER_BINDING
            const GLuint id = gl.boundSampler(gl.activeTextureUnit());
            return binding(WebGLCell::Sampler, id, gl.isSampler({id}));
        }
        case 0x8E25: {  // TRANSFORM_FEEDBACK_BINDING
            const GLuint id = gl.boundTransformFeedback();
            return binding(WebGLCell::TransformFeedback, id, gl.isTransformFeedback(id));
        }

        case 0x8E24:  // GL_TRANSFORM_FEEDBACK_ACTIVE
            return boolean(gl.transformFeedbackActive());
        case 0x8E23:  // GL_TRANSFORM_FEEDBACK_PAUSED
            return boolean(gl.transformFeedbackPaused());

        // Boolean parameters
        case 0x0BE2:  // GL_BLEND
        case 0x0B71:  // GL_DEPTH_TEST
        case 0x0B44:  // GL_CULL_FACE
        case 0x0C11:  // GL_SCISSOR_TEST
        case 0x0B90:  // GL_STENCIL_TEST
        case 0x0BD0:  // GL_DITHER
        case 0x8037:  // GL_POLYGON_OFFSET_FILL
        case 0x809E:  // GL_SAMPLE_ALPHA_TO_COVERAGE
        case 0x80A0:  // GL_SAMPLE_COVERAGE
        case 0x8C89:  // GL_RASTERIZER_DISCARD
        case 0x0B72:  // GL_DEPTH_WRITEMASK
        case 0x80AB:  // GL_SAMPLE_COVERAGE_INVERT
            return boolean(gl.getParameterBool(pname) != GL_FALSE);

        // The stencil masks are GLuint: all ones is 4294967295, not -1.
        case 0x0B93:  // GL_STENCIL_VALUE_MASK
        case 0x0B98:  // GL_STENCIL_WRITEMASK
        case 0x8CA4:  // GL_STENCIL_BACK_VALUE_MASK
        case 0x8CA5:  // GL_STENCIL_BACK_WRITEMASK
        case 0x91B0:  // MAX_SHADER_COMPILER_THREADS_KHR
            return number(static_cast<GLuint>(gl.getParameterInt(pname)));

        // Default: integer parameter.
        default:
            return number(gl.getParameterInt(pname));
    }
}

Value wrapDynamic(webgl::WebGL2RenderingContext* c, const DynamicParameter& d) {
    using Kind = DynamicParameter::Kind;
    switch (d.kind) {
        case Kind::Bool: return ev::fromBool(d.bools[0]);
        case Kind::Int32Array: return makeInt32Array(d.ints, d.count);
        case Kind::Float32Array: return makeFloat32Array(d.floats, d.count);
        case Kind::BoolArray: return hostArrayOf(d.count, [&d](size_t i) { return ev::fromBool(d.bools[i]); });
        case Kind::Object: return d.objectId ? webglObject(c, d.objectKind, d.objectId) : ev::null();
        default: return ev::fromDouble(d.number);
    }
}

} // namespace

void installWebGLQueries(ObjectBuilder& b) {
    defMethod(b, "getParameter", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        GLenum pname = u32At(a, 0);
        // The device's constants: the capabilities snapshot, no backend call.
        const webgl::vk::WebGLCaps& caps = c->caps();
        if (float f = 0.0f; caps.floatLimit(pname, f)) return ev::fromDouble(f);
        if (int64_t v = 0; caps.limit(pname, v)) return ev::fromDouble(static_cast<double>(v));
        switch (pname) {
            // String parameters
            case 0x1F02:  // GL_VERSION
                return ev::fromUtf8(caps.version);
            case 0x8B8C:  // GL_SHADING_LANGUAGE_VERSION
                return ev::fromUtf8(caps.shadingLanguageVersion);
            case 0x1F00:  // GL_VENDOR
                return ev::fromUtf8(caps.vendor);
            case 0x1F01:  // GL_RENDERER
                return ev::fromUtf8(caps.renderer);
            case 0x9245:  // UNMASKED_VENDOR_WEBGL
            case 0x9246:  // UNMASKED_RENDERER_WEBGL
                // Enums once WEBGL_debug_renderer_info is enabled.
                if (!cl->extensionEnabled("WEBGL_debug_renderer_info")) {
                    cl->syntheticError(GL_INVALID_ENUM);
                    return ev::null();
                }
                return ev::fromUtf8(pname == 0x9246 ? caps.unmaskedRenderer : caps.unmaskedVendor);
            case 0x8FBB:  // GPU_DISJOINT_EXT: nothing bro knows of makes timings meaningless
                if (!cl->extensionEnabled("EXT_disjoint_timer_query_webgl2")) cl->syntheticError(GL_INVALID_ENUM);
                return ev::fromBool(false);
            case 0x0D3A:  // GL_MAX_VIEWPORT_DIMS
                return makeInt32Array(caps.maxViewportDims, 2);
            case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
                return makeFloat32Array(caps.aliasedPointSizeRange, 2);
            case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
                return makeFloat32Array(caps.aliasedLineWidthRange, 2);
            case 0x86A3: {  // GL_COMPRESSED_TEXTURE_FORMATS: of the extensions enabled so far
                const std::vector<GLint> fmts = cl->compressedTextureFormats();
                return makeUint32Array(reinterpret_cast<const uint32_t*>(fmts.data()), fmts.size());
            }
            case 0x9247:  // MAX_CLIENT_WAIT_TIMEOUT_WEBGL
                return ev::fromDouble(WebGLBackend::kMaxClientWaitTimeoutNs);
            default: {
                const DynamicParameter d = cl->sync([pname](WebGLBackend& gl) { return dynamicParameter(gl, pname); });
                return wrapDynamic(c, d);
            }
        }
    });

    // getExtension: the extension's object (its constants, and methods for
    // the few that have them), the same one on every call; null when the
    // extension is not supported or the context is lost.
    defMethod(b, "getExtension", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (ev::isObject(nameV) || !client(c)) return ev::null();
        std::string name = ev::toUtf8(nameV);
        // Re-fetched: converting the name may run script that loses the context.
        auto* cl = client(c);
        if (!cl || !cl->getExtension(name)) return ev::null();
        return webglExtension(c, name, [c, &name] { return makeExtension(c, name); });
    });

    defMethod(b, "getIndexedParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl || a.size() < 2) return ev::null();
        uint32_t pname = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        switch (pname) {
            case 0x8C8F:  // TRANSFORM_FEEDBACK_BUFFER_BINDING
            case 0x8A28: {  // UNIFORM_BUFFER_BINDING
                const GLenum target = pname == 0x8C8F ? GL_TRANSFORM_FEEDBACK_BUFFER : GL_UNIFORM_BUFFER;
                const GLuint id = cl->sync([target, index](WebGLBackend& gl) { return gl.indexedBuffer(target, index); });
                return webglObject(c, WebGLCell::Buffer, id);
            }
            case 0x8C84:  // TRANSFORM_FEEDBACK_BUFFER_START
            case 0x8C85:  // TRANSFORM_FEEDBACK_BUFFER_SIZE
            case 0x8A29:  // UNIFORM_BUFFER_START
            case 0x8A2A:  // UNIFORM_BUFFER_SIZE
                return ev::fromDouble(static_cast<double>(
                    cl->sync([pname, index](WebGLBackend& gl) { return gl.getIndexedBufferParameter(pname, index); })));
            default:
                cl->syntheticError(GL_INVALID_ENUM);
                return ev::null();
        }
    });

    defMethod(b, "getSupportedExtensions", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        if (!client(c)) return ev::null();
        const std::vector<std::string>& exts = c->caps().extensions;
        return hostArrayOf(exts.size(), [&exts](size_t i) {
            return ev::fromUtf8(exts[i]);
        });
    });

    // The caps' precisions (every one is highp).
    defMethod(b, "getShaderPrecisionFormat", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (!client(c)) return ev::null();
        const GLenum type = u32At(a, 1);
        const bool integer = type >= 0x8DF3 && type <= 0x8DF5;  // LOW_INT .. HIGH_INT
        const webgl::vk::WebGLCaps::Precision& p = integer ? c->caps().intPrecision : c->caps().floatPrecision;
        ObjectBuilder o;
        o.set("rangeMin", ev::fromDouble(p.rangeMin));
        o.set("rangeMax", ev::fromDouble(p.rangeMax));
        o.set("precision", ev::fromDouble(p.precision));
        return o.get();
    });

    defMethod(b, "isContextLost", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        return ev::fromBool(c->isContextLost());
    });

    // What the context was granted (webgl::ContextAttributes).
    defMethod(b, "getContextAttributes", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        if (c->isContextLost()) return ev::null();
        const webgl::ContextAttributes& at = c->attributes();
        using PP = webgl::ContextAttributes::PowerPreference;
        const char* pp = at.powerPreference == PP::LowPower          ? "low-power"
                         : at.powerPreference == PP::HighPerformance ? "high-performance"
                                                                     : "default";
        ObjectBuilder o;
        o.set("alpha", ev::fromBool(at.alpha));
        o.set("depth", ev::fromBool(at.depth));
        o.set("stencil", ev::fromBool(at.stencil));
        o.set("antialias", ev::fromBool(at.antialias));
        o.set("premultipliedAlpha", ev::fromBool(at.premultipliedAlpha));
        o.set("preserveDrawingBuffer", ev::fromBool(at.preserveDrawingBuffer));
        o.set("powerPreference", ev::fromUtf8(pp));
        o.set("failIfMajorPerformanceCaveat", ev::fromBool(at.failIfMajorPerformanceCaveat));
        o.set("desynchronized", ev::fromBool(at.desynchronized));
        o.set("xrCompatible", ev::fromBool(at.xrCompatible));
        return o.get();
    });

    // --- WebGLSync: answered by the client, never the backend ---
    defMethod(b, "fenceSync", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        if (!cl->fenceSync(id, u32At(a, 0), u32At(a, 1))) return ev::null();
        return webglObject(c, WebGLCell::Sync, id);
    });
    defMethod(b, "deleteSync", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Sync);
        if (auto* cl = client(c)) cl->dropFence(id);
        forgetWebGLObject(c, WebGLCell::Sync, id);
        return ev::undefined();
    });
    defMethod(b, "clientWaitSync", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::fromDouble(0x911D);  // WAIT_FAILED
        return ev::fromDouble(cl->clientWaitSync(idOf(argAt(a, 0), WebGLCell::Sync), u32At(a, 1), numAt(a, 2)));
    });
    defMethod(b, "waitSync", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->waitSync(idOf(argAt(a, 0), WebGLCell::Sync), u32At(a, 1), numAt(a, 2));
        return ev::undefined();
    });
    defMethod(b, "getSyncParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        GLint v = 0;
        if (!cl || !cl->getSyncParameter(idOf(argAt(a, 0), WebGLCell::Sync), u32At(a, 1), v)) return ev::null();
        return ev::fromDouble(v);
    });
    defMethod(b, "isSync", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        return ev::fromBool(cl && cl->isFence(idOf(argAt(a, 0), WebGLCell::Sync)));
    });

    // --- WebGLQuery: status and result from what the replay published ---
    defMethod(b, "createQuery", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createQuery(id);
        return webglObject(c, WebGLCell::Query, id);
    });
    defMethod(b, "deleteQuery", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Query);
        if (auto* cl = client(c)) cl->deleteQuery(id);
        forgetWebGLObject(c, WebGLCell::Query, id);
        return ev::undefined();
    });
    defMethod(b, "beginQuery", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->beginQuery(u32At(a, 0), idOf(argAt(a, 1), WebGLCell::Query));
        return ev::undefined();
    });
    defMethod(b, "endQuery", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->endQuery(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "getQuery", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLenum target = u32At(a, 0), pname = u32At(a, 1);
        if (pname == 0x8864 /* QUERY_COUNTER_BITS_EXT */)
            return ev::fromDouble(cl->sync([target](WebGLBackend& gl) { return gl.queryCounterBits(target); }));
        if (pname != 0x8865 /* CURRENT_QUERY */) {
            cl->syntheticError(GL_INVALID_ENUM);
            return ev::null();
        }
        return webglObject(c, WebGLCell::Query,
                           cl->sync([target](WebGLBackend& gl) { return gl.currentQuery(target); }));
    });
    defMethod(b, "getQueryParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Query);
        const GLenum pname = u32At(a, 1);
        webgl::QueryStatus status = webgl::QueryStatus::Refused;
        uint64_t v = 0;
        // Answered from the publication unless the backend must: a query it
        // refuses (its error), or a result not available yet (a wait).
        const bool held = cl->queryStatus(id, status, v);
        if (held && status != webgl::QueryStatus::Refused) {
            if (pname == GL_QUERY_RESULT_AVAILABLE) return ev::fromBool(status == webgl::QueryStatus::Available);
            if (pname != GL_QUERY_RESULT) {
                cl->syntheticError(GL_INVALID_ENUM);
                return ev::null();
            }
            if (status == webgl::QueryStatus::Available) return ev::fromDouble(static_cast<double>(v));
        }
        if (!cl->sync([&](WebGLBackend& gl) { return gl.getQueryParameter(id, pname, v); })) return ev::null();
        if (pname == GL_QUERY_RESULT_AVAILABLE) return ev::fromBool(v != 0);
        return ev::fromDouble(static_cast<double>(v));
    });
    defMethod(b, "isQuery", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Query);
        return ev::fromBool(cl && cl->sync([id](WebGLBackend& gl) { return gl.isQuery(id); }) != GL_FALSE);
    });

    // --- Internalformat ---
    defMethod(b, "getInternalformatParameter", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        GLenum target = u32At(a, 0);
        GLenum internalformat = u32At(a, 1);
        GLenum pname = u32At(a, 2);
        if (target != GL_RENDERBUFFER || pname != 0x80A9 /* SAMPLES */) {
            cl->syntheticError(GL_INVALID_ENUM);
            return ev::null();
        }
        const std::vector<GLint>& samples = c->caps().samples(internalformat);
        return makeInt32Array(samples.data(), samples.size());
    });

    // --- WebGL2 parameter queries: round trips ---
    defMethod(b, "getTexParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLenum target = u32At(a, 0), pname = u32At(a, 1);
        webgl::TexParameterValue v;
        if (!cl || !cl->sync([&](WebGLBackend& gl) { return gl.getTexParameter(target, pname, v); }))
            return ev::null();
        switch (v.kind) {
            case webgl::TexParameterValue::Kind::Bool: return ev::fromBool(v.i != 0);
            case webgl::TexParameterValue::Kind::Float: return ev::fromDouble(v.f);
            default: return ev::fromDouble(v.i);
        }
    });

    defMethod(b, "getFramebufferAttachmentParameter", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLenum target = u32At(a, 0), attachment = u32At(a, 1), pname = u32At(a, 2);
        WebGLBackend::AttachmentParameter p;
        if (!cl ||
            !cl->sync([&](WebGLBackend& gl) {
                return gl.getFramebufferAttachmentParameter(target, attachment, pname, p);
            }) ||
            p.isNull)
            return ev::null();
        if (p.objectType == 0x1702 /* TEXTURE */) return webglObject(c, WebGLCell::Texture, p.objectName);
        if (p.objectType == GL_RENDERBUFFER) return webglObject(c, WebGLCell::Renderbuffer, p.objectName);
        return ev::fromDouble(p.value);
    });

    defMethod(b, "getRenderbufferParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLenum target = u32At(a, 0), pname = u32At(a, 1);
        return cl ? ev::fromDouble(
                        cl->sync([&](WebGLBackend& gl) { return gl.getRenderbufferParameter(target, pname); }))
                  : ev::null();
    });

    defMethod(b, "getBufferParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLenum target = u32At(a, 0), pname = u32At(a, 1);
        GLint v = 0;
        if (!cl || !cl->sync([&](WebGLBackend& gl) { return gl.getBufferParameter(target, pname, v); }))
            return ev::null();
        return ev::fromDouble(v);
    });

    defMethod(b, "getVertexAttrib", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLuint index = u32At(a, 0);
        const GLenum pname = u32At(a, 1);
        webgl::GLValue v;
        if (!cl || !cl->sync([&](WebGLBackend& gl) { return gl.getVertexAttrib(index, pname, v); }))
            return ev::null();
        return glValue(c, v);
    });

    defMethod(b, "getVertexAttribOffset", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLuint index = u32At(a, 0);
        const GLenum pname = u32At(a, 1);
        return ev::fromDouble(
            cl ? static_cast<double>(cl->sync([&](WebGLBackend& gl) { return gl.getVertexAttribOffset(index, pname); }))
               : 0);
    });

    defMethod(b, "getUniform", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const webgl::WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        const webgl::WebGLUniformLocation loc = locOf(argAt(a, 1));
        webgl::GLValue v;
        if (!cl || !cl->sync([&](WebGLBackend& gl) { return gl.getUniform(p, loc, v); })) return ev::null();
        return glValue(c, v);
    });

    // --- WebXR ---
    // There is no XR device, so makeXRCompatible rejects with AbortError.
    defMethod(b, "makeXRCompatible", 0, [](webgl::WebGL2RenderingContext*, std::span<const Value>) {
        ev::Persistent p{ev::createPromise()};
        ev::Persistent err{hostMakeDomError("AbortError", "WebXR is not supported")};
        ev::rejectPromise(p.get(), err.get());
        return p.get();
    });
}

}  // namespace bro::bronze_host

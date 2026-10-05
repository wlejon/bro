// getParameter / getExtension and friends — query dispatch implementations
// for WebGL2 pnames and extensions. An unknown pname falls to the generic
// int path; an unknown extension returns null.
//
// Array-shaped pnames answer the typed array the WebGL IDL names
// (Int32Array for VIEWPORT, Float32Array for DEPTH_RANGE, Uint32Array for
// COMPRESSED_TEXTURE_FORMATS, ...), and sequence<GLboolean> ones a real Array.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_values.h"

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

// The object an *_BINDING query names: null for none, or for one no longer
// live (deleted, or never bound).
Value bindingObject(webgl::WebGL2RenderingContext* c, uint32_t kind, GLuint id, bool exists) {
    return id != 0 && exists ? webglObject(c, kind, id) : ev::null();
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

} // namespace

void installWebGLQueries(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("getParameter", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        GLenum pname = u32At(a, 0);
        switch (pname) {
            // String parameters
            case 0x1F02:  // GL_VERSION
                return ev::fromUtf8("WebGL 2.0");
            case 0x8B8C:  // GL_SHADING_LANGUAGE_VERSION
                return ev::fromUtf8("WebGL GLSL ES 3.00");
            case 0x1F00:  // GL_VENDOR
                return ev::fromUtf8("Bro");
            case 0x1F01:  // GL_RENDERER
                return ev::fromUtf8("Bro Vulkan");

            // Float parameters
            case 0x0B73:  // GL_DEPTH_CLEAR_VALUE
            case 0x0B21:  // GL_LINE_WIDTH
            case 0x80AA:  // GL_SAMPLE_COVERAGE_VALUE
            case 0x8066:  // GL_POLYGON_OFFSET_FACTOR
            case 0x2A00:  // GL_POLYGON_OFFSET_UNITS
            case 0x84FF:  // MAX_TEXTURE_MAX_ANISOTROPY_EXT
                return ev::fromDouble(gl->getParameterFloat(pname));

            // Int[4]
            case 0x0BA2:    // GL_VIEWPORT
            case 0x0C10: {  // GL_SCISSOR_BOX
                GLint v[4] = {0, 0, 0, 0};
                gl->getParameterInt4(pname, v);
                return makeInt32Array(v, 4);
            }
            // Int[2]
            case 0x0D3A: {  // GL_MAX_VIEWPORT_DIMS
                GLint v[2] = {0, 0};
                gl->getParameterInt2(pname, v);
                return makeInt32Array(v, 2);
            }
            // Float[2]
            case 0x846D:    // GL_ALIASED_POINT_SIZE_RANGE
            case 0x846E:    // GL_ALIASED_LINE_WIDTH_RANGE
            case 0x0B70: {  // GL_DEPTH_RANGE
                GLfloat v[2] = {0, 0};
                gl->getParameterFloat2(pname, v);
                return makeFloat32Array(v, 2);
            }
            // Float[4]
            case 0x0C22:    // GL_COLOR_CLEAR_VALUE
            case 0x8005: {  // GL_BLEND_COLOR
                GLfloat v[4] = {0, 0, 0, 0};
                gl->getParameterFloat4(pname, v);
                return makeFloat32Array(v, 4);
            }
            // Boolean[4]
            case 0x0C23: {  // GL_COLOR_WRITEMASK
                GLboolean v[4] = {0, 0, 0, 0};
                gl->getParameterBool4(pname, v);
                return hostArrayOf(4, [v](size_t i) {
                    return ev::fromBool(v[i] != GL_FALSE);
                });
            }

            // WebGL-only pixel-store state.
            case 0x9240:  // UNPACK_FLIP_Y_WEBGL
            case 0x9241:  // UNPACK_PREMULTIPLY_ALPHA_WEBGL
                return ev::fromBool(gl->getPixelStorei(pname) != 0);
            case 0x9243:  // UNPACK_COLORSPACE_CONVERSION_WEBGL
                return ev::fromDouble(gl->getPixelStorei(pname));

            // Object-binding queries.
            case 0x8894: case 0x8895: case 0x88ED: case 0x88EF:
            case 0x8A28: case 0x8C8F: case 0x8F36: case 0x8F37: {  // *_BUFFER_BINDING
                const GLuint id = gl->boundBuffer(bufferBindingTarget(pname));
                return bindingObject(c, WebGLCell::Buffer, id, gl->isBuffer({id}));
            }
            case 0x8069: case 0x8514: case 0x806A: case 0x8C1D: {  // TEXTURE_BINDING_*
                const GLuint id = gl->boundTexture(textureBindingTarget(pname));
                return bindingObject(c, WebGLCell::Texture, id, gl->isTexture({id}));
            }
            case 0x8B8D: {  // CURRENT_PROGRAM
                const GLuint id = gl->currentProgram();
                return bindingObject(c, WebGLCell::Program, id, gl->isProgram({id}));
            }
            case 0x8CA6: {  // FRAMEBUFFER_BINDING / DRAW_FRAMEBUFFER_BINDING
                const GLuint id = gl->drawFramebufferBinding();
                return bindingObject(c, WebGLCell::Framebuffer, id, gl->isFramebuffer({id}));
            }
            case 0x8CAA: {  // READ_FRAMEBUFFER_BINDING
                const GLuint id = gl->readFramebufferBinding();
                return bindingObject(c, WebGLCell::Framebuffer, id, gl->isFramebuffer({id}));
            }
            case 0x8CA7: {  // RENDERBUFFER_BINDING
                const GLuint id = gl->renderbufferBinding();
                return bindingObject(c, WebGLCell::Renderbuffer, id, gl->isRenderbuffer({id}));
            }
            case 0x85B5: {  // VERTEX_ARRAY_BINDING
                const GLuint id = gl->currentVertexArray();
                return bindingObject(c, WebGLCell::VertexArray, id, gl->isVertexArray({id}));
            }
            case 0x8919: {  // SAMPLER_BINDING
                const GLuint id = gl->boundSampler(gl->activeTextureUnit());
                return bindingObject(c, WebGLCell::Sampler, id, gl->isSampler({id}));
            }
            case 0x8E25: {  // TRANSFORM_FEEDBACK_BINDING
                const GLuint id = gl->boundTransformFeedback();
                return bindingObject(c, WebGLCell::TransformFeedback, id, gl->isTransformFeedback(id));
            }

            // The compressed formats of the extensions enabled so far.
            case 0x86A3: {  // GL_COMPRESSED_TEXTURE_FORMATS
                const std::vector<GLint> fmts = gl->compressedTextureFormats();
                return makeUint32Array(reinterpret_cast<const uint32_t*>(fmts.data()), fmts.size());
            }

            case 0x9247:  // MAX_CLIENT_WAIT_TIMEOUT_WEBGL
                return ev::fromDouble(WebGLBackend::kMaxClientWaitTimeoutNs);

            case 0x8E24:  // GL_TRANSFORM_FEEDBACK_ACTIVE
                return ev::fromBool(gl->transformFeedbackActive());
            case 0x8E23:  // GL_TRANSFORM_FEEDBACK_PAUSED
                return ev::fromBool(gl->transformFeedbackPaused());

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
                return ev::fromBool(gl->getParameterBool(pname) != GL_FALSE);

            // 64-bit limits.
            case 0x8A30:  // GL_MAX_UNIFORM_BLOCK_SIZE
            case 0x8A31:  // GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS
            case 0x8A33:  // GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS
            case 0x8D6B:  // GL_MAX_ELEMENT_INDEX
            case 0x9111:  // GL_MAX_SERVER_WAIT_TIMEOUT
                return ev::fromDouble(static_cast<double>(gl->getParameterInt64(pname)));
            case 0x84FD:  // GL_MAX_TEXTURE_LOD_BIAS
                return ev::fromDouble(gl->getParameterFloat(pname));

            // Default: integer parameter (all the MAX_* limits included).
            default:
                return ev::fromDouble(gl->getParameterInt(pname));
        }
    });

    // getExtension: the extension's object (its constants, and methods for
    // the few that have them), the same one on every call; null when the
    // extension is not supported or the context is lost.
    b.def("getExtension", 1, [c](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (ev::isObject(nameV) || !live(c)) return ev::null();
        std::string name = ev::toUtf8(nameV);
        // Re-fetched: converting the name may run script that loses the context.
        auto* gl = live(c);
        if (!gl || !gl->enableExtension(name)) return ev::null();
        return webglExtension(c, name, [c, &name] { return makeExtension(c, name); });
    });

    b.def("getIndexedParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl || a.size() < 2) return ev::null();
        uint32_t pname = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        switch (pname) {
            case 0x8C8F:  // TRANSFORM_FEEDBACK_BUFFER_BINDING
                return webglObject(c, WebGLCell::Buffer, gl->indexedBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, index));
            case 0x8A28:  // UNIFORM_BUFFER_BINDING
                return webglObject(c, WebGLCell::Buffer, gl->indexedBuffer(GL_UNIFORM_BUFFER, index));
            case 0x8C84:  // TRANSFORM_FEEDBACK_BUFFER_START
            case 0x8C85:  // TRANSFORM_FEEDBACK_BUFFER_SIZE
            case 0x8A29:  // UNIFORM_BUFFER_START
            case 0x8A2A:  // UNIFORM_BUFFER_SIZE
                return ev::fromDouble(static_cast<double>(gl->getIndexedBufferParameter(pname, index)));
            default:
                gl->setSyntheticError(GL_INVALID_ENUM);
                return ev::null();
        }
    });

    b.def("getSupportedExtensions", 0, [c](Value, std::span<const Value>) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        const std::vector<std::string> exts = gl->supportedExtensions();
        return hostArrayOf(exts.size(), [&exts](size_t i) {
            return ev::fromUtf8(exts[i]);
        });
    });

    // Every precision is highp: shaders compile with precision qualifiers
    // dropped (32-bit floats and ints throughout).
    b.def("getShaderPrecisionFormat", 2, [c](Value, std::span<const Value> a) {
        if (!live(c)) return ev::null();
        const GLenum type = u32At(a, 1);
        const bool integer = type >= 0x8DF3 && type <= 0x8DF5;  // LOW_INT .. HIGH_INT
        ObjectBuilder o;
        o.set("rangeMin", ev::fromDouble(integer ? 31 : 127));
        o.set("rangeMax", ev::fromDouble(integer ? 30 : 127));
        o.set("precision", ev::fromDouble(integer ? 0 : 23));
        return o.get();
    });

    b.def("isContextLost", 0, [c](Value, std::span<const Value>) {
        return ev::fromBool(c->isContextLost());
    });

    b.def("getContextAttributes", 0, [c](Value, std::span<const Value>) {
        if (c->isContextLost()) return ev::null();
        ObjectBuilder o;
        o.set("alpha", ev::fromBool(true));
        o.set("depth", ev::fromBool(true));
        o.set("stencil", ev::fromBool(true));
        o.set("antialias", ev::fromBool(false));
        o.set("premultipliedAlpha", ev::fromBool(true));
        o.set("preserveDrawingBuffer", ev::fromBool(false));
        Value pp = ev::fromUtf8("default");
        o.set("powerPreference", pp);
        o.set("failIfMajorPerformanceCaveat", ev::fromBool(false));
        o.set("desynchronized", ev::fromBool(false));
        return o.get();
    });

    // --- WebGLSync ---
    b.def("fenceSync", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Sync, gl->fenceSync(u32At(a, 0), u32At(a, 1))) : ev::null();
    });
    b.def("deleteSync", 1, [c](Value, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Sync);
        if (auto* gl = live(c)) gl->deleteSync(id);
        forgetWebGLObject(c, WebGLCell::Sync, id);
        return ev::undefined();
    });
    b.def("clientWaitSync", 3, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::fromDouble(0x911D);  // WAIT_FAILED
        return ev::fromDouble(gl->clientWaitSync(idOf(argAt(a, 0), WebGLCell::Sync), u32At(a, 1), numAt(a, 2)));
    });
    b.def("waitSync", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->waitSync(idOf(argAt(a, 0), WebGLCell::Sync), u32At(a, 1), numAt(a, 2));
        return ev::undefined();
    });
    b.def("getSyncParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        GLint v = 0;
        if (!gl || !gl->getSyncParameter(idOf(argAt(a, 0), WebGLCell::Sync), u32At(a, 1), v)) return ev::null();
        return ev::fromDouble(v);
    });
    b.def("isSync", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isSync(idOf(argAt(a, 0), WebGLCell::Sync)) != GL_FALSE);
    });

    // --- WebGLQuery ---
    b.def("createQuery", 0, [c](Value, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Query, gl->createQuery()) : ev::null();
    });
    b.def("deleteQuery", 1, [c](Value, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Query);
        if (auto* gl = live(c)) gl->deleteQuery(id);
        forgetWebGLObject(c, WebGLCell::Query, id);
        return ev::undefined();
    });
    b.def("beginQuery", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->beginQuery(u32At(a, 0), idOf(argAt(a, 1), WebGLCell::Query));
        return ev::undefined();
    });
    b.def("endQuery", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->endQuery(u32At(a, 0));
        return ev::undefined();
    });
    b.def("getQuery", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        if (u32At(a, 1) != 0x8865 /* CURRENT_QUERY */) {
            gl->setSyntheticError(GL_INVALID_ENUM);
            return ev::null();
        }
        return webglObject(c, WebGLCell::Query, gl->currentQuery(u32At(a, 0)));
    });
    b.def("getQueryParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        const GLenum pname = u32At(a, 1);
        GLuint v = 0;
        if (!gl || !gl->getQueryParameter(idOf(argAt(a, 0), WebGLCell::Query), pname, v)) return ev::null();
        if (pname == 0x8867 /* QUERY_RESULT_AVAILABLE */) return ev::fromBool(v != 0);
        return ev::fromDouble(v);
    });
    b.def("isQuery", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isQuery(idOf(argAt(a, 0), WebGLCell::Query)) != GL_FALSE);
    });

    // --- Internalformat ---
    b.def("getInternalformatParameter", 3, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        GLenum target = u32At(a, 0);
        GLenum internalformat = u32At(a, 1);
        GLenum pname = u32At(a, 2);
        if (target != GL_RENDERBUFFER || pname != 0x80A9 /* SAMPLES */) {
            gl->setSyntheticError(GL_INVALID_ENUM);
            return ev::null();
        }
        const std::vector<GLint> samples = gl->supportedSampleCounts(internalformat);
        return makeInt32Array(samples.data(), samples.size());
    });

    // --- WebGL2 parameter queries ---
    b.def("getTexParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        webgl::TexParameterValue v;
        if (!gl || !gl->getTexParameter(u32At(a, 0), u32At(a, 1), v)) return ev::null();
        switch (v.kind) {
            case webgl::TexParameterValue::Kind::Bool: return ev::fromBool(v.i != 0);
            case webgl::TexParameterValue::Kind::Float: return ev::fromDouble(v.f);
            default: return ev::fromDouble(v.i);
        }
    });

    b.def("getFramebufferAttachmentParameter", 3, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        WebGLBackend::AttachmentParameter p;
        if (!gl || !gl->getFramebufferAttachmentParameter(u32At(a, 0), u32At(a, 1), u32At(a, 2), p) || p.isNull)
            return ev::null();
        if (p.objectType == 0x1702 /* TEXTURE */) return webglObject(c, WebGLCell::Texture, p.objectName);
        if (p.objectType == GL_RENDERBUFFER) return webglObject(c, WebGLCell::Renderbuffer, p.objectName);
        return ev::fromDouble(p.value);
    });

    b.def("getRenderbufferParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return gl ? ev::fromDouble(gl->getRenderbufferParameter(u32At(a, 0), u32At(a, 1))) : ev::null();
    });

    b.def("getBufferParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        GLint v = 0;
        if (!gl || !gl->getBufferParameter(u32At(a, 0), u32At(a, 1), v)) return ev::null();
        return ev::fromDouble(v);
    });

    b.def("getVertexAttrib", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        webgl::GLValue v;
        if (!gl || !gl->getVertexAttrib(u32At(a, 0), u32At(a, 1), v)) return ev::null();
        return glValue(c, v);
    });

    b.def("getVertexAttribOffset", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromDouble(gl ? static_cast<double>(gl->getVertexAttribOffset(u32At(a, 0), u32At(a, 1))) : 0);
    });

    b.def("getUniform", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        webgl::GLValue v;
        if (!gl || !gl->getUniform({idOf(argAt(a, 0), WebGLCell::Program)}, locOf(argAt(a, 1)), v))
            return ev::null();
        return glValue(c, v);
    });

    // --- WebXR ---
    // There is no XR device, so makeXRCompatible rejects with AbortError.
    b.def("makeXRCompatible", 0, [](Value, std::span<const Value>) {
        ev::Persistent p{ev::createPromise()};
        ev::Persistent err{hostMakeDomError("AbortError", "WebXR is not supported")};
        ev::rejectPromise(p.get(), err.get());
        return p.get();
    });
}

}  // namespace bro::bronze_host

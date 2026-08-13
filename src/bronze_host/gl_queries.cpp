// getParameter / getExtension and friends — the bronze twin of
// src/js/webgl2_bindings_queries.cpp, dispatch case for dispatch case. What
// that file answers, this answers; what it refuses (an unknown pname falls to
// the generic int path, an unknown extension is null), this refuses the same
// way — mirrored, not invented.
//
// Array-shaped pnames (VIEWPORT, COMPRESSED_TEXTURE_FORMATS, ...) answer a
// PSEUDO-array: a plain object with numeric keys and a `length` property,
// because the embed API builds objects, not Array exotics. Indexing and
// length reads work; Array.prototype methods (indexOf and friends) do not —
// a named approximation, carried in the module README.

#include "bronze_host/gl_internal.h"

#include <string>

namespace bro::bronze_host {

namespace {

// {0: v[0], ..., length: n} — see the pseudo-array note above.
template <typename T>
Value makeNumberList(const T* v, size_t n) {
    ObjectBuilder o;
    for (size_t i = 0; i < n; ++i) {
        o.obj.set(ev::setElement(o.get(), static_cast<uint32_t>(i),
                                 ev::fromDouble(static_cast<double>(v[i]))));
    }
    o.set("length", ev::fromDouble(static_cast<double>(n)));
    return o.get();
}

}  // namespace

void installGlQueries(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("getParameter", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        GLenum pname = u32At(a, 0);
        switch (pname) {
            // String parameters — same fixed strings the QuickJS binding
            // reports, so three.js's version sniffing sees one engine.
            case 0x1F02:  // GL_VERSION
                return ev::fromUtf8("WebGL 2.0");
            case 0x8B8C:  // GL_SHADING_LANGUAGE_VERSION
                return ev::fromUtf8("WebGL GLSL ES 3.00");
            case 0x1F01:  // GL_RENDERER
            case 0x1F00:  // GL_VENDOR
                return ev::fromUtf8(gl->getParameterString(pname));

            // Float parameters
            case 0x0B73:  // GL_DEPTH_CLEAR_VALUE
            case 0x0B21:  // GL_LINE_WIDTH
            case 0x80AA:  // GL_SAMPLE_COVERAGE_VALUE
            case 0x8066:  // GL_POLYGON_OFFSET_FACTOR
            case 0x2A00:  // GL_POLYGON_OFFSET_UNITS
                return ev::fromDouble(gl->getParameterFloat(pname));

            // Int[4]
            case 0x0BA2:    // GL_VIEWPORT
            case 0x0C10: {  // GL_SCISSOR_BOX
                GLint v[4] = {0, 0, 0, 0};
                glGetIntegerv(pname, v);
                return makeNumberList(v, 4);
            }
            // Int[2] — two ints; the scalar default path would smash the stack.
            case 0x0D3A: {  // GL_MAX_VIEWPORT_DIMS
                GLint v[2] = {0, 0};
                glGetIntegerv(pname, v);
                return makeNumberList(v, 2);
            }
            // Float[2]
            case 0x846D:    // GL_ALIASED_POINT_SIZE_RANGE
            case 0x846E:    // GL_ALIASED_LINE_WIDTH_RANGE
            case 0x0B70: {  // GL_DEPTH_RANGE
                GLfloat v[2] = {0, 0};
                glGetFloatv(pname, v);
                return makeNumberList(v, 2);
            }
            // Float[4]
            case 0x0C22:    // GL_COLOR_CLEAR_VALUE
            case 0x8005: {  // GL_BLEND_COLOR
                GLfloat v[4] = {0, 0, 0, 0};
                glGetFloatv(pname, v);
                return makeNumberList(v, 4);
            }
            // Boolean[4]
            case 0x0C23: {  // GL_COLOR_WRITEMASK
                GLboolean v[4] = {0, 0, 0, 0};
                glGetBooleanv(pname, v);
                ObjectBuilder o;
                for (uint32_t i = 0; i < 4; ++i) {
                    o.obj.set(ev::setElement(o.get(), i, ev::fromBool(v[i] != GL_FALSE)));
                }
                o.set("length", ev::fromDouble(4));
                return o.get();
            }

            // WebGL-only pixel-store state — shadow answers, not GL enums.
            case 0x9240:  // UNPACK_FLIP_Y_WEBGL
                return ev::fromBool(gl->unpackFlipY() != GL_FALSE);
            case 0x9241:  // UNPACK_PREMULTIPLY_ALPHA_WEBGL
                return ev::fromBool(gl->unpackPremultiplyAlpha() != GL_FALSE);
            case 0x9243:  // UNPACK_COLORSPACE_CONVERSION_WEBGL
                return ev::fromDouble(gl->unpackColorspaceConversion());

            // Object-binding queries: the QuickJS binding answers null
            // (unbound) because it cannot re-wrap; same answer, same reason.
            case 0x8894:  // ARRAY_BUFFER_BINDING
            case 0x8895:  // ELEMENT_ARRAY_BUFFER_BINDING
            case 0x8B8D:  // CURRENT_PROGRAM
            case 0x8CA6:  // FRAMEBUFFER_BINDING
            case 0x8CA7:  // RENDERBUFFER_BINDING
            case 0x8069:  // TEXTURE_BINDING_2D
            case 0x8514:  // TEXTURE_BINDING_CUBE_MAP
            case 0x85B5:  // VERTEX_ARRAY_BINDING
            case 0x8919:  // SAMPLER_BINDING
            case 0x88ED:  // PIXEL_PACK_BUFFER_BINDING
            case 0x88EF:  // PIXEL_UNPACK_BUFFER_BINDING
            case 0x8E25:  // TRANSFORM_FEEDBACK_BINDING
                return ev::null();

            // Compressed formats the driver actually probed at creation.
            case 0x86A3: {  // GL_COMPRESSED_TEXTURE_FORMATS
                const auto& fmts = gl->compressedTextureFormats();
                return makeNumberList(fmts.data(), fmts.size());
            }

            case 0x9247:  // MAX_CLIENT_WAIT_TIMEOUT_WEBGL
                return ev::fromDouble(webgl::WebGL2RenderingContext::kMaxClientWaitTimeoutNs);

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
                return ev::fromBool(gl->getParameterBool(pname) != GL_FALSE);

            // Default: integer parameter (all the MAX_* limits included).
            default:
                return ev::fromDouble(gl->getParameterInt(pname));
        }
    });

    // getExtension: the same names, the same constant sets, the same null for
    // anything the context does not report — copied from the QuickJS binding,
    // which copied them from the WebGL extension specs.
    b.def("getExtension", 1, [c](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (ev::isObject(nameV)) return ev::null();
        std::string name = ev::toUtf8(nameV);
        if (!live(c)->getExtension(name)) return ev::null();
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
        } else if (name == "EXT_texture_filter_anisotropic") {
            def("TEXTURE_MAX_ANISOTROPY_EXT", 0x84FE);
            def("MAX_TEXTURE_MAX_ANISOTROPY_EXT", 0x84FF);
        }
        // BRO_buffer_map exists in the QuickJS layer; the mapping API is not
        // bound here, so the extension is not advertised as an object either —
        // but getExtension(name) above already answered from the context's own
        // list, so an unknown-to-us supported name still returns a truthy
        // (empty) object, the WebGL convention for "present, no constants".
        return o.get();
    });

    b.def("getSupportedExtensions", 0, [c](Value, std::span<const Value>) {
        auto exts = live(c)->getSupportedExtensions();
        ObjectBuilder o;
        for (size_t i = 0; i < exts.size(); ++i) {
            Value s = ev::fromUtf8(exts[i]);
            o.obj.set(ev::setElement(o.get(), static_cast<uint32_t>(i), s));
        }
        o.set("length", ev::fromDouble(static_cast<double>(exts.size())));
        return o.get();
    });

    b.def("getShaderPrecisionFormat", 2, [](Value, std::span<const Value>) {
        // Hardcoded highp float — the QuickJS binding's answer verbatim.
        ObjectBuilder o;
        o.set("rangeMin", ev::fromDouble(127));
        o.set("rangeMax", ev::fromDouble(127));
        o.set("precision", ev::fromDouble(23));
        return o.get();
    });

    b.def("isContextLost", 0, [](Value, std::span<const Value>) {
        return ev::fromBool(false);
    });

    b.def("getContextAttributes", 0, [](Value, std::span<const Value>) {
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
}

}  // namespace bro::bronze_host

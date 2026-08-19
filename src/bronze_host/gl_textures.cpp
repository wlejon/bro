// Textures — the bronze twin of src/js/webgl2_bindings_textures.cpp for the
// 2D + cube-map surface three.js's texture system drives: bind/params/upload/
// mipmap/texStorage2D, plus the compressed 2D uploads a KTX/DDS loader feeds.
//
// The DOM-source overloads are here too — texImage2D(target, level,
// internalformat, format, type, source) and texSubImage2D(target, level,
// xoffset, yoffset, format, type, source) — because they are the pair
// three.js's WebGLTextures actually calls for a texture built from an image:
// the 9-arg forms are for DataTexture and friends. `source` is a host Image
// (host_image.cpp) or an ImageData-shaped { width, height, data }.
//
// Upload pointers come from embed::typedArrayInfo and live only until the
// next bronze allocation — each is handed to its GL call (which copies into
// the driver) in the same statement chain with nothing allocating in between.
// An Image's pixels are host memory and would survive, but resolveSource
// deliberately hands both kinds back through one type so no caller can start
// depending on which it got.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "util/log.h"

namespace bro::bronze_host {

namespace {

// The pixels behind a DOM-shaped texture source, as one triple. RGBA8, tightly
// packed, top-down (row 0 first) — which is what an image decode produces and
// what the context's UNPACK_FLIP_Y_WEBGL shadow state then flips for a caller
// that asked, exactly as it does for a client-memory typed array.
//
// THE POINTER MAY BE HEAP-BORROWED: for an ImageData-shaped source it points
// into the bronze heap and dies at the next bronze allocation, so the caller's
// GL call must be the very next thing that happens.
struct SourcePixels {
    const uint8_t* data = nullptr;
    GLsizei width = 0;
    GLsizei height = 0;
    explicit operator bool() const { return data != nullptr; }
};

SourcePixels resolveSource(Value source, const char* who) {
    if (const HostImage* img = hostImageOf(source)) {
        if (img->rgba.empty()) {
            // A broken image: HTML gives it zero natural dimensions and no
            // pixels, so there is nothing to upload and the texture keeps
            // whatever it had. Warned, because a silently unchanged texture is
            // indistinguishable from a working one that happens to be black.
            LOG_WARN("bronze_host: %s was given an Image with no pixels (src=%s)", who,
                     img->src.c_str());
            return {};
        }
        return {img->rgba.data(), img->width, img->height};
    }

    if (dom::Element* el = hostElementOf(source)) {
        if (auto* scene = static_cast<bro::canvas::CanvasScene*>(el->canvasScene())) {
            int w = std::atoi(el->getAttribute("width").c_str());
            int h = std::atoi(el->getAttribute("height").c_str());
            if (w <= 0) w = 300;
            if (h <= 0) h = 150;
            const uint8_t* px = scene->snapshotPixels(w, h);
            if (px) {
                return {px, static_cast<GLsizei>(w), static_cast<GLsizei>(h)};
            }
        }
        static const std::vector<uint8_t> s_dummy(4, 255);
        return {s_dummy.data(), 1, 1};
    }

    // ImageData-shaped { width, height, data }: the duck type bro's own canvas
    // and createImageBitmap paths already consume (src/js/imagebitmap_bindings.cpp),
    // matched here so a texture built from raw RGBA needs no new object kind.
    if (ev::isObject(source)) {
        ev::Persistent root(source);
        Value widthV = ev::getProperty(root.get(), "width");
        Value heightV = ev::getProperty(root.get(), "height");
        if (!ev::isObject(widthV) && !ev::isObject(heightV) && !ev::isUndefined(widthV) &&
            !ev::isUndefined(heightV)) {
            const GLsizei w = static_cast<GLsizei>(ev::toDouble(widthV));
            const GLsizei h = static_cast<GLsizei>(ev::toDouble(heightV));
            // LAST, and deliberately: every allocating read is above this line,
            // so the view's pointer is still valid when the caller's GL call
            // consumes it on the very next statement.
            Value dataV = ev::getProperty(root.get(), "data");
            if (auto info = ev::typedArrayInfo(dataV)) {
                const uint64_t need = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4u;
                if (w > 0 && h > 0 && info.byteLength >= need) {
                    return {info.data, w, h};
                }
            }
        }
    }

    LOG_ERROR("bronze_host: %s DOM-source overload needs an Image, Canvas, or an "
              "ImageData-shaped { width, height, data }",
              who);
    return {};
}

}  // namespace

void installGlTextures(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("createTexture", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Texture, live(c)->createTexture().id);
    });
    b.def("deleteTexture", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteTexture({idOf(argAt(a, 0), GlCell::Texture)});
        return ev::undefined();
    });
    b.def("bindTexture", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindTexture(u32At(a, 0), {idOf(argAt(a, 1), GlCell::Texture)});
        return ev::undefined();
    });
    b.def("isTexture", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isTexture({idOf(argAt(a, 0), GlCell::Texture)}) !=
                            GL_FALSE);
    });
    b.def("activeTexture", 1, [c](Value, std::span<const Value> a) {
        live(c)->activeTexture(u32At(a, 0));
        return ev::undefined();
    });
    b.def("texParameteri", 3, [c](Value, std::span<const Value> a) {
        live(c)->texParameteri(u32At(a, 0), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("texParameterf", 3, [c](Value, std::span<const Value> a) {
        live(c)->texParameterf(u32At(a, 0), u32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    // texImage2D(target, level, internalformat, width, height, border,
    //            format, type, data|null). Data is a typed array or
    // null/undefined; a 6-arg DOM-source call is a named refusal (above).
    b.def("texImage2D", 9, [c](Value, std::span<const Value> a) {
        // Short calls are padded to arity with undefined, so the 6-arg
        // DOM-source call announces itself by an OBJECT where the 9-arg form
        // has `border` (a number). That is the whole overload discriminator,
        // and it is exact: `border` is required to be 0 in both WebGL versions,
        // so a number there is never a source and a source there is never a
        // border.
        if (ev::isObject(argAt(a, 5))) {
            // texImage2D(target, level, internalformat, format, type, source)
            GLenum domTarget = u32At(a, 0);
            GLint domLevel = i32At(a, 1);
            GLint domInternalformat = i32At(a, 2);
            GLenum domFormat = u32At(a, 3);
            GLenum domType = u32At(a, 4);
            SourcePixels src = resolveSource(argAt(a, 5), "texImage2D");
            if (src) {
                // Nothing between resolveSource and here allocates on the
                // bronze heap, which is what keeps an ImageData-backed pointer
                // valid; the context copies the bytes into the driver.
                live(c)->texImage2D(domTarget, domLevel, domInternalformat, src.width,
                                    src.height, /*border=*/0, domFormat, domType, src.data);
            }
            return ev::undefined();
        }
        GLenum target = u32At(a, 0);
        GLint level = i32At(a, 1);
        GLint internalformat = i32At(a, 2);
        GLsizei width = i32At(a, 3);
        GLsizei height = i32At(a, 4);
        GLint border = i32At(a, 5);
        GLenum format = u32At(a, 6);
        GLenum type = u32At(a, 7);
        Value data = argAt(a, 8);
        if (ev::isUndefined(data) || ev::isNull(data)) {
            live(c)->texImage2D(target, level, internalformat, width, height, border,
                                format, type, nullptr);
        } else if (auto info = ev::typedArrayInfo(data)) {
            live(c)->texImage2D(target, level, internalformat, width, height, border,
                                format, type, info.data);
        }
        return ev::undefined();
    });

    // texSubImage2D(target, level, xoffset, yoffset, width, height, format,
    //               type, data). Same typed-array-only stance.
    b.def("texSubImage2D", 9, [c](Value, std::span<const Value> a) {
        // Same padding logic as texImage2D above: the 7-arg DOM-source call
        // puts its source at index 6, where the 9-arg form has `format`, a
        // numeric enum. An object there is the 7-arg form.
        //
        // This is the overload three.js reaches on the WebGL2 path it takes by
        // default: WebGLTextures allocates with texStorage2D and then fills
        // level 0 with texSubImage2D(target, 0, 0, 0, format, type, image).
        if (ev::isObject(argAt(a, 6))) {
            // texSubImage2D(target, level, xoffset, yoffset, format, type, source)
            GLenum domTarget = u32At(a, 0);
            GLint domLevel = i32At(a, 1);
            GLint domX = i32At(a, 2);
            GLint domY = i32At(a, 3);
            GLenum domFormat = u32At(a, 4);
            GLenum domType = u32At(a, 5);
            SourcePixels src = resolveSource(argAt(a, 6), "texSubImage2D");
            if (src) {
                live(c)->texSubImage2D(domTarget, domLevel, domX, domY, src.width,
                                       src.height, domFormat, domType, src.data);
            }
            return ev::undefined();
        }
        GLenum target = u32At(a, 0);
        GLint level = i32At(a, 1);
        GLint xoffset = i32At(a, 2);
        GLint yoffset = i32At(a, 3);
        GLsizei width = i32At(a, 4);
        GLsizei height = i32At(a, 5);
        GLenum format = u32At(a, 6);
        GLenum type = u32At(a, 7);
        if (auto info = ev::typedArrayInfo(argAt(a, 8))) {
            live(c)->texSubImage2D(target, level, xoffset, yoffset, width, height, format,
                                   type, info.data);
        }
        return ev::undefined();
    });

    b.def("texStorage2D", 5, [c](Value, std::span<const Value> a) {
        live(c)->texStorage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                              i32At(a, 4));
        return ev::undefined();
    });

    // The 3D family. Not an optional extra: WebGLState's constructor builds
    // its empty TEXTURE_2D_ARRAY / TEXTURE_3D textures with texImage3D, so a
    // renderer cannot even be CONSTRUCTED without it — its absence was the
    // first compiled WebGLRenderer's "undefined is not a function".
    b.def("texImage3D", 10, [c](Value, std::span<const Value> a) {
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        bufferBytes(argAt(a, 9), &data, &len, &elemSize);  // null pixels stay null
        live(c)->texImage3D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                            i32At(a, 4), i32At(a, 5), i32At(a, 6), u32At(a, 7),
                            u32At(a, 8), data);
        return ev::undefined();
    });
    b.def("texSubImage3D", 11, [c](Value, std::span<const Value> a) {
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        bufferBytes(argAt(a, 10), &data, &len, &elemSize);
        live(c)->texSubImage3D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                               i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7),
                               u32At(a, 8), u32At(a, 9), data);
        return ev::undefined();
    });
    b.def("texStorage3D", 6, [c](Value, std::span<const Value> a) {
        live(c)->texStorage3D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                              i32At(a, 4), i32At(a, 5));
        return ev::undefined();
    });
    b.def("generateMipmap", 1, [c](Value, std::span<const Value> a) {
        live(c)->generateMipmap(u32At(a, 0));
        return ev::undefined();
    });

    // Compressed uploads, with the WebGL2 srcOffset/srcLengthOverride tail in
    // ELEMENT units of the source view — the same clamping the QuickJS
    // binding performs before the context's block-size validation runs.
    b.def("compressedTexImage2D", 7, [c](Value, std::span<const Value> a) {
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 6), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t srcOffset = static_cast<size_t>(u32At(a, 7));
            if (srcOffset > elemCount) srcOffset = elemCount;
            size_t count = elemCount - srcOffset;
            if (a.size() >= 9 && !ev::isUndefined(a[8])) {
                size_t l = static_cast<size_t>(u32At(a, 8));
                if (l < count) count = l;
            }
            live(c)->compressedTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                          i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                          data + srcOffset * elemSize, count * elemSize);
        }
        return ev::undefined();
    });
    b.def("compressedTexSubImage2D", 8, [c](Value, std::span<const Value> a) {
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 7), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t srcOffset = static_cast<size_t>(u32At(a, 8));
            if (srcOffset > elemCount) srcOffset = elemCount;
            size_t count = elemCount - srcOffset;
            if (a.size() >= 10 && !ev::isUndefined(a[9])) {
                size_t l = static_cast<size_t>(u32At(a, 9));
                if (l < count) count = l;
            }
            live(c)->compressedTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2),
                                             i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                             u32At(a, 6), data + srcOffset * elemSize,
                                             count * elemSize);
        }
        return ev::undefined();
    });

    b.def("copyTexImage2D", 8, [c](Value, std::span<const Value> a) {
        live(c)->copyTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                                i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
    b.def("copyTexSubImage2D", 8, [c](Value, std::span<const Value> a) {
        live(c)->copyTexSubImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                                   i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });

    // --- WebGLSampler ---
    b.def("createSampler", 0, [c](Value, std::span<const Value>) {
        return wrapSampler(live(c)->createSampler());
    });
    b.def("deleteSampler", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteSampler(samplerOf(argAt(a, 0)));
        return ev::undefined();
    });
    b.def("bindSampler", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindSampler(u32At(a, 0), samplerOf(argAt(a, 1)));
        return ev::undefined();
    });
    b.def("samplerParameteri", 3, [c](Value, std::span<const Value> a) {
        live(c)->samplerParameteri(samplerOf(argAt(a, 0)), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("samplerParameterf", 3, [c](Value, std::span<const Value> a) {
        live(c)->samplerParameterf(samplerOf(argAt(a, 0)), u32At(a, 1),
                                   static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    b.def("getSamplerParameter", 2, [c](Value, std::span<const Value> a) {
        GLenum pname = u32At(a, 1);
        if (pname == 0x813A /* TEXTURE_MAX_ANISOTROPY_EXT */ ||
            pname == 0x8501 /* TEXTURE_MIN_LOD */ ||
            pname == 0x8502 /* TEXTURE_MAX_LOD */) {
            return ev::fromDouble(live(c)->getSamplerParameterf(samplerOf(argAt(a, 0)), pname));
        }
        return ev::fromDouble(live(c)->getSamplerParameteri(samplerOf(argAt(a, 0)), pname));
    });
    b.def("isSampler", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isSampler(samplerOf(argAt(a, 0))) != GL_FALSE);
    });
}

}  // namespace bro::bronze_host

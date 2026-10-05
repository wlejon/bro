// Textures — the bronze host implementation for the
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
// next bronze allocation — each is handed to the backend (which copies it)
// in the same statement chain with nothing allocating in between.
// An Image's pixels are host memory and would survive, but resolveSource
// deliberately hands both kinds back through one type so no caller can start
// depending on which it got.

#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_image.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_globals_internal.h"

#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "layout/el_video.h"
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
// upload must be the very next thing that happens.
struct SourcePixels {
    const uint8_t* data = nullptr;
    GLsizei width = 0;
    GLsizei height = 0;
    explicit operator bool() const { return data != nullptr; }
};

SourcePixels resolveSource(Value sourceIn, const char* who) {
    // Rooted: each probe below may allocate (hostImageOf and hostElementOf
    // read a property by name), and the next probe must see the value's
    // current address.
    const Rooted source(sourceIn);
    if (const HostImageBitmap* bmp = hostImageBitmapOf(source)) {
        if (bmp->closed || bmp->pixels.empty()) {
            LOG_WARN("bronze_host: %s was given an ImageBitmap with no pixels", who);
            return {};
        }
        return {bmp->pixels.data(), static_cast<GLsizei>(bmp->width), static_cast<GLsizei>(bmp->height)};
    }

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
            // The bitmap's own size (the scene's surface), not the
            // attributes: a bitmaprenderer canvas shows its ImageBitmap at
            // that bitmap's size, and snapshotPixels answers null for any
            // size but the surface's.
            const int w = scene->width();
            const int h = scene->height();
            const uint8_t* px = (w > 0 && h > 0) ? scene->snapshotPixels(w, h) : nullptr;
            if (px) return {px, static_cast<GLsizei>(w), static_cast<GLsizei>(h)};
        }
        if (auto* vc = el->videoControl()) {
            int vw = 0, vh = 0;
            const uint8_t* px = vc->currentFrameRgba(&vw, &vh);
            if (px && vw > 0 && vh > 0) {
                return {px, static_cast<GLsizei>(vw), static_cast<GLsizei>(vh)};
            }
        }
        LOG_WARN("bronze_host: %s was given an Element with no available pixels", who);
        return {};
    }

    // ImageData-shaped { width, height, data }: the standard duck type canvas
    // and createImageBitmap paths consume,
    // matched here so a texture built from raw RGBA needs no new object kind.
    if (ev::isObject(source)) {
        ev::Persistent root(source);
        // Each dimension converts before the next read can move a string one.
        Value dimV = ev::getProperty(root.get(), "width");
        const bool hasW = !ev::isObject(dimV) && !ev::isUndefined(dimV);
        const GLsizei w = hasW ? static_cast<GLsizei>(ev::toDouble(dimV)) : 0;
        dimV = ev::getProperty(root.get(), "height");
        const bool hasH = !ev::isObject(dimV) && !ev::isUndefined(dimV);
        const GLsizei h = hasH ? static_cast<GLsizei>(ev::toDouble(dimV)) : 0;
        if (hasW && hasH) {
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


// Resolve a DOM source and hand its pixels to `upload` straight away (the
// pointer may be heap-borrowed); INVALID_VALUE when it has none.
template <typename Upload>
void uploadSource(webgl::WebGL2RenderingContext* c, Value source, const char* who, Upload&& upload) {
    SourcePixels src = resolveSource(source, who);
    if (!src) {
        if (auto* gl = live(c)) gl->setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    // Nothing between resolveSource and the upload allocates on the bronze
    // heap, which keeps an ImageData-backed pointer valid.
    upload(src);
}

// The bytes of a typed-array (or ArrayBuffer) upload source from element
// `srcOffset` on. A srcOffset past the end is INVALID_VALUE (`rangeError`).
struct ViewBytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool rangeError = false;
    explicit operator bool() const { return data != nullptr; }
};

ViewBytes viewBytes(webgl::WebGL2RenderingContext* c, Value v, Value srcOffset) {
    ViewBytes out;
    const uint8_t* data = nullptr;
    size_t len = 0, elemSize = 1;
    if (!ev::typedArrayInfo(v) || !bufferBytes(v, &data, &len, &elemSize)) return out;
    const size_t offset = ev::isUndefined(srcOffset) ? 0 : static_cast<size_t>(ev::toDouble(srcOffset));
    if (offset > len / elemSize) {
        if (auto* gl = live(c)) gl->setSyntheticError(GL_INVALID_VALUE);
        out.rangeError = true;
        return out;
    }
    out.data = data + offset * elemSize;
    out.size = len - offset * elemSize;
    // A zero-length view still names client memory, not "no data".
    if (out.size == 0) out.data = data;
    return out;
}

// A compressed upload's view at `index`, then srcOffset and
// srcLengthOverride in elements.
ViewBytes compressedView(webgl::WebGL2RenderingContext* c, std::span<const Value> a, size_t index) {
    const uint8_t* data = nullptr;
    size_t len = 0, elemSize = 1;
    ViewBytes out;
    if (!bufferBytes(argAt(a, index), &data, &len, &elemSize)) {
        if (auto* gl = live(c)) gl->setSyntheticError(GL_INVALID_VALUE);
        return out;
    }
    const size_t elemCount = len / elemSize;
    const size_t srcOffset = a.size() > index + 1 && !ev::isUndefined(a[index + 1]) ? u32At(a, index + 1) : 0;
    const bool hasOverride = a.size() > index + 2 && !ev::isUndefined(a[index + 2]);
    const size_t override = hasOverride ? u32At(a, index + 2) : 0;
    if (srcOffset > elemCount || (hasOverride && override > elemCount - srcOffset)) {
        if (auto* gl = live(c)) gl->setSyntheticError(GL_INVALID_VALUE);
        return out;
    }
    const size_t count = hasOverride ? override : elemCount - srcOffset;
    out.data = data + srcOffset * elemSize;
    out.size = count * elemSize;
    return out;
}

// Pixels of a DOM source go to 2D images only: a 3D image from one is
// refused, by name, rather than read as something it is not.
void refuseDomSource3D(webgl::WebGL2RenderingContext* c, const char* who) {
    LOG_ERROR("bronze_host: %s from a DOM source is not supported; upload a typed array", who);
    if (auto* gl = live(c)) gl->setSyntheticError(GL_INVALID_OPERATION);
}

}  // namespace

void installWebGLTextures(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("createTexture", 0, [c](Value, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Texture, gl->createTexture().id) : ev::null();
    });
    b.def("deleteTexture", 1, [c](Value, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Texture);
        if (auto* gl = live(c)) gl->deleteTexture({id});
        forgetWebGLObject(c, WebGLCell::Texture, id);
        return ev::undefined();
    });
    b.def("bindTexture", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->bindTexture(u32At(a, 0), {idOf(argAt(a, 1), WebGLCell::Texture)});
        return ev::undefined();
    });
    b.def("isTexture", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isTexture({idOf(argAt(a, 0), WebGLCell::Texture)}) != GL_FALSE);
    });
    b.def("activeTexture", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->activeTexture(u32At(a, 0));
        return ev::undefined();
    });
    b.def("texParameteri", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->texParameteri(u32At(a, 0), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("texParameterf", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->texParameterf(u32At(a, 0), u32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    // texImage2D: the 6-arg DOM-source form, or the 9/10-arg form whose
    // pixels are null, a PIXEL_UNPACK_BUFFER offset, a typed array (with
    // WebGL 2's srcOffset), or a DOM source of the given size.
    b.def("texImage2D", 9, [c](Value, std::span<const Value> a) {
        // Short calls are padded to arity with undefined, so the 6-arg
        // DOM-source call announces itself by an OBJECT where the 9-arg form
        // has `border` (a number, required to be 0).
        if (ev::isObject(argAt(a, 5))) {
            uploadSource(c, argAt(a, 5), "texImage2D", [&](const SourcePixels& src) {
                if (auto* gl = live(c))
                    gl->texImage2DSource(u32At(a, 0), i32At(a, 1), i32At(a, 2), -1, -1, u32At(a, 3),
                                         u32At(a, 4), src.data, static_cast<uint32_t>(src.width),
                                         static_cast<uint32_t>(src.height));
            });
            return ev::undefined();
        }
        const GLenum target = u32At(a, 0), format = u32At(a, 6), type = u32At(a, 7);
        const GLint level = i32At(a, 1), internalformat = i32At(a, 2), border = i32At(a, 5);
        const GLsizei width = i32At(a, 3), height = i32At(a, 4);
        Value data = argAt(a, 8);
        if (ev::isUndefined(data) || ev::isNull(data)) {
            if (auto* gl = live(c))
                gl->texImage2D(target, level, internalformat, width, height, border, format, type, nullptr, 0);
        } else if (ev::isNumber(data)) {
            if (auto* gl = live(c))
                gl->texImage2DFromPBO(target, level, internalformat, width, height, border, format, type,
                                      static_cast<GLintptr>(i64At(a, 8)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 9))) {
            if (auto* gl = live(c))
                gl->texImage2D(target, level, internalformat, width, height, border, format, type, view.data,
                               view.size);
        } else if (!view.rangeError) {
            uploadSource(c, data, "texImage2D", [&](const SourcePixels& src) {
                if (auto* gl = live(c))
                    gl->texImage2DSource(target, level, internalformat, width, height, format, type, src.data,
                                         static_cast<uint32_t>(src.width), static_cast<uint32_t>(src.height));
            });
        }
        return ev::undefined();
    });

    // texSubImage2D: the 7-arg DOM-source form (three.js's default WebGL 2
    // path: texStorage2D, then this), or the 9/10-arg form.
    b.def("texSubImage2D", 9, [c](Value, std::span<const Value> a) {
        if (ev::isObject(argAt(a, 6))) {
            uploadSource(c, argAt(a, 6), "texSubImage2D", [&](const SourcePixels& src) {
                if (auto* gl = live(c))
                    gl->texSubImage2DSource(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), -1, -1,
                                            u32At(a, 4), u32At(a, 5), src.data, static_cast<uint32_t>(src.width),
                                            static_cast<uint32_t>(src.height));
            });
            return ev::undefined();
        }
        const GLenum target = u32At(a, 0), format = u32At(a, 6), type = u32At(a, 7);
        const GLint level = i32At(a, 1), xoffset = i32At(a, 2), yoffset = i32At(a, 3);
        const GLsizei width = i32At(a, 4), height = i32At(a, 5);
        Value data = argAt(a, 8);
        if (ev::isNumber(data)) {
            if (auto* gl = live(c))
                gl->texSubImage2DFromPBO(target, level, xoffset, yoffset, width, height, format, type,
                                         static_cast<GLintptr>(i64At(a, 8)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 9))) {
            if (auto* gl = live(c))
                gl->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, view.data,
                                  view.size);
        } else if (ev::isObject(data) && !view.rangeError) {
            uploadSource(c, data, "texSubImage2D", [&](const SourcePixels& src) {
                if (auto* gl = live(c))
                    gl->texSubImage2DSource(target, level, xoffset, yoffset, width, height, format, type,
                                            src.data, static_cast<uint32_t>(src.width),
                                            static_cast<uint32_t>(src.height));
            });
        } else if (!view.rangeError) {
            if (auto* gl = live(c))
                gl->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, nullptr, 0);
        }
        return ev::undefined();
    });

    b.def("texStorage2D", 5, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->texStorage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                             i32At(a, 4));
        return ev::undefined();
    });

    // The 3D family. Not an optional extra: WebGLState's constructor builds
    // its empty TEXTURE_2D_ARRAY / TEXTURE_3D textures with texImage3D, so a
    // renderer cannot even be CONSTRUCTED without it.
    b.def("texImage3D", 10, [c](Value, std::span<const Value> a) {
        const GLenum target = u32At(a, 0), format = u32At(a, 7), type = u32At(a, 8);
        const GLint level = i32At(a, 1), internalformat = i32At(a, 2), border = i32At(a, 6);
        const GLsizei width = i32At(a, 3), height = i32At(a, 4), depth = i32At(a, 5);
        Value data = argAt(a, 9);
        if (ev::isUndefined(data) || ev::isNull(data)) {
            if (auto* gl = live(c))
                gl->texImage3D(target, level, internalformat, width, height, depth, border, format, type, nullptr,
                               0);
        } else if (ev::isNumber(data)) {
            if (auto* gl = live(c))
                gl->texImage3DFromPBO(target, level, internalformat, width, height, depth, border, format, type,
                                      static_cast<GLintptr>(i64At(a, 9)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 10))) {
            if (auto* gl = live(c))
                gl->texImage3D(target, level, internalformat, width, height, depth, border, format, type,
                               view.data, view.size);
        } else if (!view.rangeError) {
            refuseDomSource3D(c, "texImage3D");
        }
        return ev::undefined();
    });
    b.def("texSubImage3D", 11, [c](Value, std::span<const Value> a) {
        const GLenum target = u32At(a, 0), format = u32At(a, 8), type = u32At(a, 9);
        const GLint level = i32At(a, 1), x = i32At(a, 2), y = i32At(a, 3), z = i32At(a, 4);
        const GLsizei width = i32At(a, 5), height = i32At(a, 6), depth = i32At(a, 7);
        Value data = argAt(a, 10);
        if (ev::isNumber(data)) {
            if (auto* gl = live(c))
                gl->texSubImage3DFromPBO(target, level, x, y, z, width, height, depth, format, type,
                                         static_cast<GLintptr>(i64At(a, 10)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 11))) {
            if (auto* gl = live(c))
                gl->texSubImage3D(target, level, x, y, z, width, height, depth, format, type, view.data,
                                  view.size);
        } else if (ev::isObject(data) && !view.rangeError) {
            refuseDomSource3D(c, "texSubImage3D");
        } else if (!view.rangeError) {
            if (auto* gl = live(c))
                gl->texSubImage3D(target, level, x, y, z, width, height, depth, format, type, nullptr, 0);
        }
        return ev::undefined();
    });
    b.def("texStorage3D", 6, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->texStorage3D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                             i32At(a, 4), i32At(a, 5));
        return ev::undefined();
    });
    b.def("generateMipmap", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->generateMipmap(u32At(a, 0));
        return ev::undefined();
    });

    // Compressed uploads: a view (with the WebGL 2 srcOffset /
    // srcLengthOverride tail, in elements), or imageSize + a byte offset
    // into the bound PIXEL_UNPACK_BUFFER.
    b.def("compressedTexImage2D", 7, [c](Value, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 6))) {
            if (auto* gl = live(c))
                gl->compressedTexImageFromPBO(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4), 1,
                                              i32At(a, 5), i32At(a, 6), static_cast<GLintptr>(i64At(a, 7)),
                                              false);
        } else if (ViewBytes view = compressedView(c, a, 6)) {
            if (auto* gl = live(c))
                gl->compressedTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4),
                                         i32At(a, 5), view.data, view.size);
        }
        return ev::undefined();
    });
    b.def("compressedTexSubImage2D", 8, [c](Value, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 7))) {
            if (auto* gl = live(c))
                gl->compressedTexSubImageFromPBO(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), 0,
                                                 i32At(a, 4), i32At(a, 5), 1, u32At(a, 6), i32At(a, 7),
                                                 static_cast<GLintptr>(i64At(a, 8)), false);
        } else if (ViewBytes view = compressedView(c, a, 7)) {
            if (auto* gl = live(c))
                gl->compressedTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                            i32At(a, 5), u32At(a, 6), view.data, view.size);
        }
        return ev::undefined();
    });
    b.def("compressedTexImage3D", 8, [c](Value, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 7))) {
            if (auto* gl = live(c))
                gl->compressedTexImageFromPBO(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4),
                                              i32At(a, 5), i32At(a, 6), i32At(a, 7),
                                              static_cast<GLintptr>(i64At(a, 8)), true);
        } else if (ViewBytes view = compressedView(c, a, 7)) {
            if (auto* gl = live(c))
                gl->compressedTexImage3D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4),
                                         i32At(a, 5), i32At(a, 6), view.data, view.size);
        }
        return ev::undefined();
    });
    b.def("compressedTexSubImage3D", 10, [c](Value, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 9))) {
            if (auto* gl = live(c))
                gl->compressedTexSubImageFromPBO(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                                 i32At(a, 5), i32At(a, 6), i32At(a, 7), u32At(a, 8), i32At(a, 9),
                                                 static_cast<GLintptr>(i64At(a, 10)), true);
        } else if (ViewBytes view = compressedView(c, a, 9)) {
            if (auto* gl = live(c))
                gl->compressedTexSubImage3D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                            i32At(a, 5), i32At(a, 6), i32At(a, 7), u32At(a, 8), view.data,
                                            view.size);
        }
        return ev::undefined();
    });

    b.def("copyTexImage2D", 8, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->copyTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                               i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
    b.def("copyTexSubImage2D", 8, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->copyTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                                  i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
    b.def("copyTexSubImage3D", 9, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->copyTexSubImage3D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                  i32At(a, 5), i32At(a, 6), i32At(a, 7), i32At(a, 8));
        return ev::undefined();
    });

    // --- WebGLSampler ---
    b.def("createSampler", 0, [c](Value, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Sampler, gl->createSampler().id) : ev::null();
    });
    b.def("deleteSampler", 1, [c](Value, std::span<const Value> a) {
        const webgl::WebGLSampler s = samplerOf(argAt(a, 0));
        if (auto* gl = live(c)) gl->deleteSampler(s);
        forgetWebGLObject(c, WebGLCell::Sampler, s.id);
        return ev::undefined();
    });
    b.def("bindSampler", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->bindSampler(u32At(a, 0), samplerOf(argAt(a, 1)));
        return ev::undefined();
    });
    b.def("samplerParameteri", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->samplerParameteri(samplerOf(argAt(a, 0)), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("samplerParameterf", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->samplerParameterf(samplerOf(argAt(a, 0)), u32At(a, 1),
                                  static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    b.def("getSamplerParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        GLenum pname = u32At(a, 1);
        if (pname == 0x813A /* TEXTURE_MIN_LOD */ || pname == 0x813B /* TEXTURE_MAX_LOD */ ||
            pname == 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */) {
            return ev::fromDouble(gl->getSamplerParameterf(samplerOf(argAt(a, 0)), pname));
        }
        return ev::fromDouble(gl->getSamplerParameteri(samplerOf(argAt(a, 0)), pname));
    });
    b.def("isSampler", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isSampler(samplerOf(argAt(a, 0))) != GL_FALSE);
    });
}

}  // namespace bro::bronze_host

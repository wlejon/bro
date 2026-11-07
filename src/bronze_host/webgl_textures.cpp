// Textures — the bronze host implementation for the
// 2D + cube-map surface three.js's texture system drives: bind/params/upload/
// mipmap/texStorage2D, plus the compressed 2D uploads a KTX/DDS loader feeds.
//
// The DOM-source overloads are here too — texImage2D(target, level,
// internalformat, format, type, source) and texSubImage2D(target, level,
// xoffset, yoffset, format, type, source) — because they are the pair
// three.js's WebGLTextures actually calls for a texture built from an image:
// the 9-arg forms are for DataTexture and friends. `source` is a host Image
// (host_image.cpp), an ImageBitmap, a canvas or video element, or an
// ImageData-shaped { width, height, data }.
//
// Every call is encoded by the context's client and replays later, so its
// pixels are the client's from the call on: a typed array's bytes are copied
// into the command (embed::typedArrayInfo's pointer lives only until the
// next bronze allocation, and nothing allocates between the read and the
// copy); a decoded image (Image, ImageBitmap) is held by reference, since it
// never changes; a canvas snapshot, a video frame and ImageData are copied.

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
    // The decoded image `data` points into, when it is one: held, not copied.
    std::shared_ptr<const render::DecodedImage> decoded;
    explicit operator bool() const { return data != nullptr; }
};

SourcePixels resolveSource(Value sourceIn, const char* who) {
    // Rooted: each probe below may allocate (hostImageOf and hostElementOf
    // read a property by name), and the next probe must see the value's
    // current address.
    const Rooted source(sourceIn);
    if (const HostImageBitmap* bmp = hostImageBitmapOf(source)) {
        if (!bmp->rgba()) {
            LOG_WARN("bronze_host: %s was given an ImageBitmap with no pixels", who);
            return {};
        }
        return {bmp->rgba(), static_cast<GLsizei>(bmp->width), static_cast<GLsizei>(bmp->height), bmp->pixels};
    }

    if (const HostImage* img = hostImageOf(source)) {
        if (!img->rgba()) {
            // A broken image: HTML gives it zero natural dimensions and no
            // pixels, so there is nothing to upload and the texture keeps
            // whatever it had. Warned, because a silently unchanged texture is
            // indistinguishable from a working one that happens to be black.
            LOG_WARN("bronze_host: %s was given an Image with no pixels (src=%s)", who,
                     img->src.c_str());
            return {};
        }
        return {img->rgba(), img->width, img->height, img->pixels};
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


// Resolve a DOM source and hand its pixels, held or copied, to `upload`
// straight away (the pointer may be heap-borrowed); INVALID_VALUE when it has
// none.
template <typename Upload>
void uploadSource(webgl::WebGL2RenderingContext* c, Value source, const char* who, Upload&& upload) {
    SourcePixels src = resolveSource(source, who);
    auto* cl = client(c);
    if (!cl) return;
    if (!src) {
        cl->syntheticError(GL_INVALID_VALUE);
        return;
    }
    const auto w = static_cast<uint32_t>(src.width), h = static_cast<uint32_t>(src.height);
    // Nothing between resolveSource and the copy allocates on the bronze
    // heap, which keeps an ImageData-backed pointer valid.
    upload(*cl, src.decoded ? webgl::WebGLClient::heldPixels(
                                  std::shared_ptr<const uint8_t>(src.decoded, src.data), w, h)
                            : cl->copiedPixels(src.data, w, h));
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
        if (auto* cl = client(c)) cl->syntheticError(GL_INVALID_VALUE);
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
        if (auto* cl = client(c)) cl->syntheticError(GL_INVALID_VALUE);
        return out;
    }
    const size_t elemCount = len / elemSize;
    const size_t srcOffset = a.size() > index + 1 && !ev::isUndefined(a[index + 1]) ? u32At(a, index + 1) : 0;
    const bool hasOverride = a.size() > index + 2 && !ev::isUndefined(a[index + 2]);
    const size_t override = hasOverride ? u32At(a, index + 2) : 0;
    if (srcOffset > elemCount || (hasOverride && override > elemCount - srcOffset)) {
        if (auto* cl = client(c)) cl->syntheticError(GL_INVALID_VALUE);
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
    if (auto* cl = client(c)) cl->syntheticError(GL_INVALID_OPERATION);
}

}  // namespace


void installWebGLTextures(ObjectBuilder& b) {
    using webgl::WebGLSampler;
    using webgl::WebGLTexture;
    defMethod(b, "createTexture", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createTexture(id);
        return webglObject(c, WebGLCell::Texture, id);
    });
    defMethod(b, "deleteTexture", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Texture);
        if (auto* cl = client(c)) cl->deleteTexture(WebGLTexture{id});
        forgetWebGLObject(c, WebGLCell::Texture, id);
        return ev::undefined();
    });
    defMethod(b, "bindTexture", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->bindTexture(u32At(a, 0), WebGLTexture{idOf(argAt(a, 1), WebGLCell::Texture)});
        return ev::undefined();
    });
    defMethod(b, "isTexture", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const WebGLTexture tex{idOf(argAt(a, 0), WebGLCell::Texture)};
        return ev::fromBool(cl && cl->sync([tex](WebGLBackend& gl) { return gl.isTexture(tex); }) != GL_FALSE);
    });
    defMethod(b, "activeTexture", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->activeTexture(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "texParameteri", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->texParameteri(u32At(a, 0), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "texParameterf", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->texParameterf(u32At(a, 0), u32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    // texImage2D: the 6-arg DOM-source form, or the 9/10-arg form whose
    // pixels are null, a PIXEL_UNPACK_BUFFER offset, a typed array (with
    // WebGL 2's srcOffset), or a DOM source of the given size.
    defMethod(b, "texImage2D", 9, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        // Short calls are padded to arity with undefined, so the 6-arg
        // DOM-source call announces itself by an OBJECT where the 9-arg form
        // has `border` (a number, required to be 0).
        if (ev::isObject(argAt(a, 5))) {
            uploadSource(c, argAt(a, 5), "texImage2D", [&](webgl::WebGLClient& cl, webgl::rec::PixelRef px) {
                cl.texImage2DSource(u32At(a, 0), i32At(a, 1), i32At(a, 2), -1, -1, u32At(a, 3), u32At(a, 4),
                                    std::move(px));
            });
            return ev::undefined();
        }
        const GLenum target = u32At(a, 0), format = u32At(a, 6), type = u32At(a, 7);
        const GLint level = i32At(a, 1), internalformat = i32At(a, 2), border = i32At(a, 5);
        const GLsizei width = i32At(a, 3), height = i32At(a, 4);
        Value data = argAt(a, 8);
        if (ev::isUndefined(data) || ev::isNull(data)) {
            if (auto* cl = client(c))
                cl->texImage2D(target, level, internalformat, width, height, border, format, type,
                               static_cast<const void*>(nullptr), size_t{0});
        } else if (ev::isNumber(data)) {
            if (auto* cl = client(c))
                cl->texImage2DFromPBO(target, level, internalformat, width, height, border, format, type,
                                      static_cast<GLintptr>(i64At(a, 8)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 9))) {
            if (auto* cl = client(c))
                cl->texImage2D(target, level, internalformat, width, height, border, format, type,
                               static_cast<const void*>(view.data), view.size);
        } else if (!view.rangeError) {
            uploadSource(c, data, "texImage2D", [&](webgl::WebGLClient& cl, webgl::rec::PixelRef px) {
                cl.texImage2DSource(target, level, internalformat, width, height, format, type, std::move(px));
            });
        }
        return ev::undefined();
    });

    // texSubImage2D: the 7-arg DOM-source form (three.js's default WebGL 2
    // path: texStorage2D, then this), or the 9/10-arg form.
    defMethod(b, "texSubImage2D", 9, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (ev::isObject(argAt(a, 6))) {
            uploadSource(c, argAt(a, 6), "texSubImage2D", [&](webgl::WebGLClient& cl, webgl::rec::PixelRef px) {
                cl.texSubImage2DSource(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), -1, -1, u32At(a, 4),
                                       u32At(a, 5), std::move(px));
            });
            return ev::undefined();
        }
        const GLenum target = u32At(a, 0), format = u32At(a, 6), type = u32At(a, 7);
        const GLint level = i32At(a, 1), xoffset = i32At(a, 2), yoffset = i32At(a, 3);
        const GLsizei width = i32At(a, 4), height = i32At(a, 5);
        Value data = argAt(a, 8);
        if (ev::isNumber(data)) {
            if (auto* cl = client(c))
                cl->texSubImage2DFromPBO(target, level, xoffset, yoffset, width, height, format, type,
                                         static_cast<GLintptr>(i64At(a, 8)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 9))) {
            if (auto* cl = client(c))
                cl->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                                  static_cast<const void*>(view.data), view.size);
        } else if (ev::isObject(data) && !view.rangeError) {
            uploadSource(c, data, "texSubImage2D", [&](webgl::WebGLClient& cl, webgl::rec::PixelRef px) {
                cl.texSubImage2DSource(target, level, xoffset, yoffset, width, height, format, type, std::move(px));
            });
        } else if (!view.rangeError) {
            if (auto* cl = client(c))
                cl->texSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                                  static_cast<const void*>(nullptr), size_t{0});
        }
        return ev::undefined();
    });

    defMethod(b, "texStorage2D", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->texStorage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });

    // The 3D family. Not an optional extra: WebGLState's constructor builds
    // its empty TEXTURE_2D_ARRAY / TEXTURE_3D textures with texImage3D, so a
    // renderer cannot even be CONSTRUCTED without it.
    defMethod(b, "texImage3D", 10, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLenum target = u32At(a, 0), format = u32At(a, 7), type = u32At(a, 8);
        const GLint level = i32At(a, 1), internalformat = i32At(a, 2), border = i32At(a, 6);
        const GLsizei width = i32At(a, 3), height = i32At(a, 4), depth = i32At(a, 5);
        Value data = argAt(a, 9);
        if (ev::isUndefined(data) || ev::isNull(data)) {
            if (auto* cl = client(c))
                cl->texImage3D(target, level, internalformat, width, height, depth, border, format, type,
                               static_cast<const void*>(nullptr), size_t{0});
        } else if (ev::isNumber(data)) {
            if (auto* cl = client(c))
                cl->texImage3DFromPBO(target, level, internalformat, width, height, depth, border, format, type,
                                      static_cast<GLintptr>(i64At(a, 9)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 10))) {
            if (auto* cl = client(c))
                cl->texImage3D(target, level, internalformat, width, height, depth, border, format, type,
                               static_cast<const void*>(view.data), view.size);
        } else if (!view.rangeError) {
            refuseDomSource3D(c, "texImage3D");
        }
        return ev::undefined();
    });
    defMethod(b, "texSubImage3D", 11, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLenum target = u32At(a, 0), format = u32At(a, 8), type = u32At(a, 9);
        const GLint level = i32At(a, 1), x = i32At(a, 2), y = i32At(a, 3), z = i32At(a, 4);
        const GLsizei width = i32At(a, 5), height = i32At(a, 6), depth = i32At(a, 7);
        Value data = argAt(a, 10);
        if (ev::isNumber(data)) {
            if (auto* cl = client(c))
                cl->texSubImage3DFromPBO(target, level, x, y, z, width, height, depth, format, type,
                                         static_cast<GLintptr>(i64At(a, 10)));
        } else if (ViewBytes view = viewBytes(c, data, argAt(a, 11))) {
            if (auto* cl = client(c))
                cl->texSubImage3D(target, level, x, y, z, width, height, depth, format, type,
                                  static_cast<const void*>(view.data), view.size);
        } else if (ev::isObject(data) && !view.rangeError) {
            refuseDomSource3D(c, "texSubImage3D");
        } else if (!view.rangeError) {
            if (auto* cl = client(c))
                cl->texSubImage3D(target, level, x, y, z, width, height, depth, format, type,
                                  static_cast<const void*>(nullptr), size_t{0});
        }
        return ev::undefined();
    });
    defMethod(b, "texStorage3D", 6, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->texStorage3D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4), i32At(a, 5));
        return ev::undefined();
    });
    defMethod(b, "generateMipmap", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->generateMipmap(u32At(a, 0));
        return ev::undefined();
    });

    // Compressed uploads: a view (with the WebGL 2 srcOffset /
    // srcLengthOverride tail, in elements), or imageSize + a byte offset
    // into the bound PIXEL_UNPACK_BUFFER.
    defMethod(b, "compressedTexImage2D", 7, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 6))) {
            if (auto* cl = client(c))
                cl->compressedTexImageFromPBO(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4), 1,
                                              i32At(a, 5), i32At(a, 6), static_cast<GLintptr>(i64At(a, 7)), false);
        } else if (ViewBytes view = compressedView(c, a, 6)) {
            if (auto* cl = client(c))
                cl->compressedTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4),
                                         i32At(a, 5), static_cast<const void*>(view.data), view.size);
        }
        return ev::undefined();
    });
    defMethod(b, "compressedTexSubImage2D", 8, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 7))) {
            if (auto* cl = client(c))
                cl->compressedTexSubImageFromPBO(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), 0,
                                                 i32At(a, 4), i32At(a, 5), 1, u32At(a, 6), i32At(a, 7),
                                                 static_cast<GLintptr>(i64At(a, 8)), false);
        } else if (ViewBytes view = compressedView(c, a, 7)) {
            if (auto* cl = client(c))
                cl->compressedTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                            i32At(a, 5), u32At(a, 6), static_cast<const void*>(view.data), view.size);
        }
        return ev::undefined();
    });
    defMethod(b, "compressedTexImage3D", 8, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 7))) {
            if (auto* cl = client(c))
                cl->compressedTexImageFromPBO(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4),
                                              i32At(a, 5), i32At(a, 6), i32At(a, 7),
                                              static_cast<GLintptr>(i64At(a, 8)), true);
        } else if (ViewBytes view = compressedView(c, a, 7)) {
            if (auto* cl = client(c))
                cl->compressedTexImage3D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4),
                                         i32At(a, 5), i32At(a, 6), static_cast<const void*>(view.data), view.size);
        }
        return ev::undefined();
    });
    defMethod(b, "compressedTexSubImage3D", 10, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (ev::isNumber(argAt(a, 9))) {
            if (auto* cl = client(c))
                cl->compressedTexSubImageFromPBO(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                                 i32At(a, 5), i32At(a, 6), i32At(a, 7), u32At(a, 8), i32At(a, 9),
                                                 static_cast<GLintptr>(i64At(a, 10)), true);
        } else if (ViewBytes view = compressedView(c, a, 9)) {
            if (auto* cl = client(c))
                cl->compressedTexSubImage3D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4),
                                            i32At(a, 5), i32At(a, 6), i32At(a, 7), u32At(a, 8),
                                            static_cast<const void*>(view.data), view.size);
        }
        return ev::undefined();
    });

    defMethod(b, "copyTexImage2D", 8, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->copyTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3), i32At(a, 4), i32At(a, 5),
                               i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
    defMethod(b, "copyTexSubImage2D", 8, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->copyTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                  i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
    defMethod(b, "copyTexSubImage3D", 9, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->copyTexSubImage3D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                  i32At(a, 6), i32At(a, 7), i32At(a, 8));
        return ev::undefined();
    });

    // --- WebGLSampler ---
    defMethod(b, "createSampler", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createSampler(id);
        return webglObject(c, WebGLCell::Sampler, id);
    });
    defMethod(b, "deleteSampler", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const WebGLSampler s = samplerOf(argAt(a, 0));
        if (auto* cl = client(c)) cl->deleteSampler(s);
        forgetWebGLObject(c, WebGLCell::Sampler, s.id);
        return ev::undefined();
    });
    defMethod(b, "bindSampler", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->bindSampler(u32At(a, 0), samplerOf(argAt(a, 1)));
        return ev::undefined();
    });
    defMethod(b, "samplerParameteri", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->samplerParameteri(samplerOf(argAt(a, 0)), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "samplerParameterf", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->samplerParameterf(samplerOf(argAt(a, 0)), u32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    defMethod(b, "getSamplerParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLenum pname = u32At(a, 1);
        const WebGLSampler s = samplerOf(argAt(a, 0));
        if (pname == 0x813A /* TEXTURE_MIN_LOD */ || pname == 0x813B /* TEXTURE_MAX_LOD */ ||
            pname == 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */) {
            return ev::fromDouble(cl->sync([&](WebGLBackend& gl) { return gl.getSamplerParameterf(s, pname); }));
        }
        return ev::fromDouble(cl->sync([&](WebGLBackend& gl) { return gl.getSamplerParameteri(s, pname); }));
    });
    defMethod(b, "isSampler", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const WebGLSampler s = samplerOf(argAt(a, 0));
        return ev::fromBool(cl && cl->sync([s](WebGLBackend& gl) { return gl.isSampler(s); }) != GL_FALSE);
    });
}

}  // namespace bro::bronze_host

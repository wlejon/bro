#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/runtime.h"
#include "canvas/canvas_scene.h"

#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include <qjsbind/qjsbind.h>

#include "svg/svg_renderer.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include "util/object_url.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

#include "broimage/decode.h"

#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif
#include "broimage/encode.h"
#include "broimage/geometric.h"
#include "broimage/alpha.h"
#include "broimage/color.h"
#include "broimage/preproc.h"
#include "broimage/normalize.h"
#include "broimage/presets.h"
#include "broimage/tiling.h"
#include "broimage/kernels.h"
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>

namespace bro::js {

// Where relative image paths resolve, per realm.
//
// This used to be one process-global pair, but install() runs once per realm —
// the app, every system panel, every <iframe> sub-document — so whichever
// loaded last quietly took ownership of path resolution for all of them.
// System panels load after the app, which left an app's own
// `<img src="images/x.png">` resolving to `<panel dir>/images/x.png` and
// failing to open: every DOM image used as a canvas or WebGL source came back
// empty, with only a load-failure warning naming a path the app never wrote.
struct AssetBase {
    std::string path;
    const util::AssetMounts* mounts = nullptr;
};
static std::unordered_map<JSContext*, AssetBase> s_bases;
// Workers install only the kernels and carry no base of their own (see
// installKernels), so they fall back to the most recent main-thread install.
static AssetBase s_fallbackBase;

static const AssetBase& assetBaseFor(JSContext* ctx) {
    auto it = s_bases.find(ctx);
    return it != s_bases.end() ? it->second : s_fallbackBase;
}

struct ImageData {
    int width = 0;
    int height = 0;
    std::string src;
    std::vector<uint8_t> pixels; // RGBA
    bool complete = false;
    JSValue onload = JS_UNDEFINED;  // stored callback
    JSValue onerror = JS_UNDEFINED; // stored callback
    JSContext* ctx = nullptr;

    // The stored handlers hold strong refs. Release them on finalize, and mark
    // them for the cycle GC (see .gc_mark below) — `img.onload = () => img.foo`
    // is a wrapper -> callback -> wrapper cycle that would otherwise never be
    // collected.
    ~ImageData() {
        if (ctx) {
            JS_FreeValue(ctx, onload);
            JS_FreeValue(ctx, onerror);
        }
    }
};

using ID = ImageData;

// Resolve an image src path against a base directory and the engine mounts.
static std::string resolveAgainst(const AssetBase& base, const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;   // Windows C:\...
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (base.mounts) {
            std::string m = base.mounts->resolve(src);
            if (!m.empty()) return m;
        }
        return src;
    }
    if (base.path.empty()) return src;
    std::string path = base.path;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

// Resolve against whichever realm the call came from.
static std::string resolvePath(JSContext* ctx, const std::string& src) {
    return resolveAgainst(assetBaseFor(ctx), src);
}

// -------------------------------------------------------------------------
// Complex property setters/methods needing raw signatures
// -------------------------------------------------------------------------

// Adopt a decode result and fire the one event it earned. Shared by every way
// an Image can get its bytes — a file, a data: URL, an object URL — so all of
// them agree on what a broken image looks like and when the handler runs.
static JSValue finishImageLoad(JSContext* ctx, JSValueConst this_val, ID* img,
                               broimage::Image& decoded, bool ok,
                               const std::string& err) {
    // A failed decode is a *broken image*, not a 1x1 white one. broimage hands
    // back a white fallback pixel on failure; adopting it would make a missing
    // asset indistinguishable from a real image and silently paint white. Per
    // the HTML spec a broken image has zero natural dimensions and no pixels,
    // so drawImage/texImage2D of it no-ops (getImagePixels tests pixels.empty)
    // and createImageBitmap rejects.
    if (ok) {
        img->width  = decoded.width;
        img->height = decoded.height;
        img->pixels = std::move(decoded.pixels);
        LOG_INFO("Image loaded: %s (%dx%d)", img->src.c_str(), img->width, img->height);
    } else {
        img->width  = 0;
        img->height = 0;
        img->pixels.clear();
        LOG_WARN("Image load failed: %s (%s)", img->src.c_str(), err.c_str());
    }
    img->complete = true; // the fetch settled, success or not

    // Fire load/error — exactly one, never both. Route through the error funnel
    // so a throwing handler is reported and cleared rather than left pending on
    // the context for an unrelated call to trip over.
    JSValue handler = ok ? img->onload : img->onerror;
    if (JS_IsFunction(ctx, handler)) {
        JSValue func = JS_DupValue(ctx, handler);
        JSValue ret = Runtime::callJs(ctx, func, this_val, 0, nullptr,
            ErrorOrigin::listener(std::string(ok ? "load" : "error") +
                                  " on Image " + img->src));
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, func);
    }
    return JS_UNDEFINED;
}

// src setter — decodes the image via broimage and fires onload
static JSValue js_image_set_src(JSContext* ctx, JSValueConst this_val,
                                int /*argc*/, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    img->src = s;
    JS_FreeCString(ctx, s);

    broimage::Image decoded;
    std::string err;

    // A URL that carries its own bytes — a data: payload or a blob: object URL
    // — never touches the disk. Both are how a page hands an image to an <img>
    // it built itself: glTF embeds its textures as data: URLs, and an importer
    // reading files the user dropped resolves them through createObjectURL.
    // Going straight to resolvePath() treated the whole URL as a filename, so
    // both arrived as a missing file and the image came back broken.
    std::vector<uint8_t> inlineBytes;
    if (bro::util::isObjectURL(img->src) &&
        !bro::util::lookupObjectURL(img->src)) {
        // Revoked, or from a realm that has gone away. Say so, rather than
        // falling through and reporting that a file named "blob:…" is missing.
        err = "object URL is not registered (revoked?)";
        return finishImageLoad(ctx, this_val, img, decoded, false, err);
    }
    if (bro::util::inlineURLBytes(img->src, inlineBytes)) {
        bool ok = !inlineBytes.empty() &&
                  broimage::decode_memory(inlineBytes.data(), inlineBytes.size(),
                                          decoded, &err);
        if (!ok && bro::svg::looksLikeSvg(
                       reinterpret_cast<const char*>(inlineBytes.data()),
                       inlineBytes.size())) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (bro::svg::rasterizeSvgMarkup(
                    reinterpret_cast<const char*>(inlineBytes.data()),
                    inlineBytes.size(), 0, 0, w, h, rgba)) {
                decoded.width = w;
                decoded.height = h;
                decoded.channels = 4;
                decoded.pixels = std::move(rgba);
                ok = true;
            }
        }
        return finishImageLoad(ctx, this_val, img, decoded, ok, err);
    }

    // Resolve path and decode via broimage (stb-backed, RGBA-forced). Decode is
    // synchronous, so load/error fires before the setter returns.
    std::string path = resolvePath(img->ctx, img->src);
    bool ok = broimage::decode_file(path, decoded, &err);

#if BRO_WITH_WEBP
    // broimage is stb-backed and stb has no WebP, so a .webp lands here as a
    // plain decode failure. Try libwebp before calling the image broken —
    // this is the same fallback the renderer's decode path takes, and both
    // have to agree or a .webp would draw but report naturalWidth 0 (or the
    // reverse). See render/webp_image.h.
    if (!ok) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (bro::render::decodeWebPFile(path, w, h, rgba)) {
            decoded.width = w;
            decoded.height = h;
            decoded.channels = 4;
            decoded.pixels = std::move(rgba);
            ok = true;
        }
    }
#endif

    // SVG. broimage is stb-backed and stb decodes bitmaps, so a vector image
    // arrives here as "unknown image type" — which is how an ordinary
    // `<img src="icon.svg">` turns into a broken image even though bro has a
    // full SVG renderer a layer away. Rasterize it at its intrinsic size and
    // hand back pixels, so it is an image like any other from here on.
    if (!ok) {
        std::string markup;
        {
            std::ifstream f(path, std::ios::binary);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                markup = ss.str();
            }
        }
        if (bro::svg::looksLikeSvg(markup.data(), markup.size())) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (bro::svg::rasterizeSvgMarkup(markup.data(), markup.size(), 0, 0,
                                             w, h, rgba)) {
                decoded.width = w;
                decoded.height = h;
                decoded.channels = 4;
                decoded.pixels = std::move(rgba);
                ok = true;
            } else {
                err = "SVG parse/rasterize failed";
            }
        }
    }

    return finishImageLoad(ctx, this_val, img, decoded, ok, err);
}

// Pick the slot an event type maps to. Image carries one handler per type
// rather than a listener list (see addEventListener below).
static JSValue* slotForType(ID* img, const char* type) {
    if (std::string(type) == "load")  return &img->onload;
    if (std::string(type) == "error") return &img->onerror;
    return nullptr;
}

// onload / onerror setters — manage JSValue ref counting
static JSValue js_image_set_onload(JSContext* ctx, JSValueConst this_val,
                                    int /*argc*/, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img) return JS_UNDEFINED;
    if (!JS_IsUndefined(img->onload)) {
        JS_FreeValue(ctx, img->onload);
    }
    img->onload = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue js_image_set_onerror(JSContext* ctx, JSValueConst this_val,
                                     int /*argc*/, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img) return JS_UNDEFINED;
    if (!JS_IsUndefined(img->onerror)) {
        JS_FreeValue(ctx, img->onerror);
    }
    img->onerror = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

// addEventListener — "load"/"error" alias onto the onload/onerror slots. Only
// one listener per type is retained (a second add replaces the first).
static JSValue js_image_addEventListener(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (JSValue* slot = slotForType(img, type)) {
        if (!JS_IsUndefined(*slot)) JS_FreeValue(ctx, *slot);
        *slot = JS_DupValue(ctx, argv[1]);
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

// removeEventListener
static JSValue js_image_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (JSValue* slot = slotForType(img, type)) {
        if (!JS_IsUndefined(*slot)) {
            JS_FreeValue(ctx, *slot);
            *slot = JS_UNDEFINED;
        }
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

// =========================================================================
// bro.image kernels — broimage decode/encode, geometric, alpha, color,
// preproc, normalize, multi-channel stencil, tiling.
//
// The core verb kernels (reduce/map/combine/lookup/stencil/resample +
// gradient/alloc) live in brokit (src/api/image.cpp) and are installed onto
// `bro.image` by brokit::api::installAll(). This block augments that same
// object with the rest of broimage's public surface. JS naming is camelCase,
// transforms are caller-allocated (into-style: provide dst), and decode/
// encode/probe/exif return values. Mirrors the brokit glue conventions.
// =========================================================================

namespace {

struct TAView { uint8_t* data; size_t byte_len; size_t bpe; };

// Unpack a TypedArray (must be a typed array). Throws + returns false on miss.
static bool unpackTA(JSContext* ctx, JSValueConst val, const char* name, TAView* out) {
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, val, &byte_offset, &byte_len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "%s must be a TypedArray", name);
        return false;
    }
    size_t ab_len = 0;
    uint8_t* ab_ptr = JS_GetArrayBuffer(ctx, &ab_len, buf);
    JS_FreeValue(ctx, buf);
    if (!ab_ptr) {
        JS_ThrowTypeError(ctx, "%s has detached or invalid buffer", name);
        return false;
    }
    out->data = ab_ptr + byte_offset;
    out->byte_len = byte_len;
    out->bpe = bpe;
    return true;
}

// Accept either a TypedArray or an ArrayBuffer; yield a read-only byte view.
static bool getBytes(JSContext* ctx, JSValueConst val, const uint8_t** ptr, size_t* len) {
    size_t bo = 0, bl = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, val, &bo, &bl, &bpe);
    if (!JS_IsException(buf)) {
        size_t ab = 0;
        uint8_t* p = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!p) return false;
        *ptr = p + bo;
        *len = bl;
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t ab = 0;
    uint8_t* p = JS_GetArrayBuffer(ctx, &ab, val);
    if (p) { *ptr = p; *len = ab; return true; }
    return false;
}

static bool propF64(JSContext* ctx, JSValueConst obj, const char* key, double* out, double def) {
    if (!JS_IsObject(obj)) { *out = def; return true; }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); *out = def; return true; }
    int rc = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return rc == 0;
}

static bool propI32(JSContext* ctx, JSValueConst obj, const char* key, int32_t* out, int32_t def) {
    if (!JS_IsObject(obj)) { *out = def; return true; }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); *out = def; return true; }
    int rc = JS_ToInt32(ctx, out, v);
    JS_FreeValue(ctx, v);
    return rc == 0;
}

static bool propStr(JSContext* ctx, JSValueConst obj, const char* key, std::string* out) {
    out->clear();
    if (!JS_IsObject(obj)) return true;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return true; }
    const char* s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return false;
    *out = s;
    JS_FreeCString(ctx, s);
    return true;
}

// Read n floats out of a JS array into `out`. Returns false if not an array of
// at least n numeric elements.
static bool readFloats(JSContext* ctx, JSValueConst arr, float* out, int n) {
    if (!JS_IsArray(arr)) return false;
    for (int i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        double d = 0;
        int rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc) return false;
        out[i] = (float)d;
    }
    return true;
}

// Read an array-valued property into n floats; falls back to defaults on miss.
static bool propFloats(JSContext* ctx, JSValueConst obj, const char* key,
                       float* out, int n, const float* def) {
    if (def) for (int i = 0; i < n; i++) out[i] = def[i];
    if (!JS_IsObject(obj)) return def != nullptr;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return def != nullptr; }
    bool ok = readFloats(ctx, v, out, n);
    JS_FreeValue(ctx, v);
    return ok;
}

// Construct a JS TypedArray of the named class holding a copy of `bytes`.
static JSValue newTA(JSContext* ctx, const char* ctor, const void* data, size_t bytes) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)data, bytes);
    if (JS_IsException(ab)) return ab;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue c = JS_GetPropertyStr(ctx, global, ctor);
    JS_FreeValue(ctx, global);
    JSValueConst args[1] = { ab };
    JSValue arr = JS_CallConstructor(ctx, c, 1, args);
    JS_FreeValue(ctx, c);
    JS_FreeValue(ctx, ab);
    return arr;
}

static bool parseFilter(JSContext* ctx, const std::string& s, broimage::Filter def,
                        broimage::Filter* out) {
    if (s.empty())            { *out = def; return true; }
    if (s == "nearest")       { *out = broimage::Filter::Nearest;  return true; }
    if (s == "bilinear")      { *out = broimage::Filter::Bilinear; return true; }
    if (s == "bicubic")       { *out = broimage::Filter::Bicubic;  return true; }
    if (s == "lanczos3")      { *out = broimage::Filter::Lanczos3; return true; }
    if (s == "area")          { *out = broimage::Filter::Area;     return true; }
    JS_ThrowTypeError(ctx, "filter must be nearest|bilinear|bicubic|lanczos3|area");
    return false;
}

// Build {width, height, channels, pixels} from decode output.
static JSValue makeDecodeResult(JSContext* ctx, int w, int h, int channels,
                                const char* ctor, const void* data, size_t bytes) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, channels));
    JS_SetPropertyStr(ctx, obj, "pixels", newTA(ctx, ctor, data, bytes));
    return obj;
}

// -------------------------------------------------------------------------
// Decode (16-bit / float / oriented), probe, EXIF
// -------------------------------------------------------------------------

// decodeU16(src) — src is a path string or Uint8Array/ArrayBuffer of an
// encoded image. Returns {width,height,channels,pixels:Uint16Array} or null.
static JSValue img_decodeU16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "decodeU16(pathOrBytes)");
    broimage::ImageU16 out;
    std::string err;
    bool ok;
    if (JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        ok = broimage::decode_file_u16(resolvePath(ctx, s), out, &err);
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t* p; size_t n;
        if (!getBytes(ctx, argv[0], &p, &n))
            return JS_ThrowTypeError(ctx, "decodeU16: expected path string or bytes");
        ok = broimage::decode_memory_u16(p, n, out, &err);
    }
    if (!ok) return JS_NULL;
    return makeDecodeResult(ctx, out.width, out.height, out.channels,
                            "Uint16Array", out.pixels.data(),
                            out.pixels.size() * sizeof(uint16_t));
}

// decodeF32(src) — HDR / float decode. Returns pixels as Float32Array.
static JSValue img_decodeF32(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "decodeF32(pathOrBytes)");
    broimage::ImageF32 out;
    std::string err;
    bool ok;
    if (JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        ok = broimage::decode_file_f32(resolvePath(ctx, s), out, &err);
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t* p; size_t n;
        if (!getBytes(ctx, argv[0], &p, &n))
            return JS_ThrowTypeError(ctx, "decodeF32: expected path string or bytes");
        ok = broimage::decode_memory_f32(p, n, out, &err);
    }
    if (!ok) return JS_NULL;
    return makeDecodeResult(ctx, out.width, out.height, out.channels,
                            "Float32Array", out.pixels.data(),
                            out.pixels.size() * sizeof(float));
}

// decodeOriented(src) — 8-bit RGBA decode with EXIF auto-orientation applied.
static JSValue img_decodeOriented(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "decodeOriented(pathOrBytes)");
    broimage::Image out;
    std::string err;
    bool ok;
    if (JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        ok = broimage::decode_file_oriented(resolvePath(ctx, s), out, &err);
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t* p; size_t n;
        if (!getBytes(ctx, argv[0], &p, &n))
            return JS_ThrowTypeError(ctx, "decodeOriented: expected path string or bytes");
        ok = broimage::decode_memory_oriented(p, n, out, &err);
    }
    (void)ok; // oriented decode has a 1x1 fallback like decode_file
    return makeDecodeResult(ctx, out.width, out.height, out.channels,
                            "Uint8Array", out.pixels.data(), out.pixels.size());
}

// probeDimensions(bytes) — cheap header probe. Returns {width,height,channels}
// or null on an unrecognized/truncated header.
static JSValue img_probeDimensions(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "probeDimensions(bytes)");
    const uint8_t* p; size_t n;
    if (!getBytes(ctx, argv[0], &p, &n))
        return JS_ThrowTypeError(ctx, "probeDimensions: expected bytes");
    int w = 0, h = 0, c = 0;
    if (!broimage::probe_dimensions_memory(p, n, &w, &h, &c)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, c));
    return obj;
}

// readExifOrientation(src) — path string or JPEG bytes. Returns 1..8 (1 when
// absent/invalid).
static JSValue img_readExifOrientation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "readExifOrientation(pathOrBytes)");
    broimage::ExifOrientation o;
    if (JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        o = broimage::read_exif_orientation_file(resolvePath(ctx, s));
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t* p; size_t n;
        if (!getBytes(ctx, argv[0], &p, &n))
            return JS_ThrowTypeError(ctx, "readExifOrientation: expected path string or bytes");
        o = broimage::read_exif_orientation(p, n);
    }
    return JS_NewInt32(ctx, (int)o);
}

// applyExifOrientation(pixelsRGBA8, width, height, orient) — returns
// {width,height,pixels} (dims may swap for 90/270 transforms).
static JSValue img_applyExifOrientation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "applyExifOrientation(pixels, w, h, orient)");
    TAView px;
    if (!unpackTA(ctx, argv[0], "pixels", &px)) return JS_EXCEPTION;
    int32_t w, h, orient;
    if (JS_ToInt32(ctx, &w, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &orient, argv[3])) return JS_EXCEPTION;
    if (px.bpe != 1) return JS_ThrowTypeError(ctx, "applyExifOrientation: pixels must be Uint8Array (RGBA8)");
    if (w <= 0 || h <= 0 || px.byte_len < (size_t)w * h * 4)
        return JS_ThrowRangeError(ctx, "applyExifOrientation: pixels too small for w*h*4");
    broimage::Image img;
    img.width = w; img.height = h; img.channels = 4;
    img.pixels.assign(px.data, px.data + (size_t)w * h * 4);
    broimage::apply_exif_orientation(img, (broimage::ExifOrientation)orient);
    return makeDecodeResult(ctx, img.width, img.height, img.channels,
                            "Uint8Array", img.pixels.data(), img.pixels.size());
}

// -------------------------------------------------------------------------
// Encode (PNG / JPEG, file + memory)
// -------------------------------------------------------------------------

static JSValue img_encodePngFile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "encodePngFile(path, pixels, w, h, channels, strideBytes?)");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    TAView px;
    if (!unpackTA(ctx, argv[1], "pixels", &px)) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    int32_t w, h, c, stride = 0;
    if (JS_ToInt32(ctx, &w, argv[2]) || JS_ToInt32(ctx, &h, argv[3]) || JS_ToInt32(ctx, &c, argv[4])) {
        JS_FreeCString(ctx, path); return JS_EXCEPTION;
    }
    if (argc >= 6 && !JS_IsUndefined(argv[5])) { if (JS_ToInt32(ctx, &stride, argv[5])) { JS_FreeCString(ctx, path); return JS_EXCEPTION; } }
    bool ok = broimage::encode_png_file(resolvePath(ctx, path), px.data, w, h, c, stride);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

static JSValue img_encodePng(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "encodePng(pixels, w, h, channels, strideBytes?)");
    TAView px;
    if (!unpackTA(ctx, argv[0], "pixels", &px)) return JS_EXCEPTION;
    int32_t w, h, c, stride = 0;
    if (JS_ToInt32(ctx, &w, argv[1]) || JS_ToInt32(ctx, &h, argv[2]) || JS_ToInt32(ctx, &c, argv[3])) return JS_EXCEPTION;
    if (argc >= 5 && !JS_IsUndefined(argv[4])) { if (JS_ToInt32(ctx, &stride, argv[4])) return JS_EXCEPTION; }
    std::vector<uint8_t> out;
    if (!broimage::encode_png_memory(out, px.data, w, h, c, stride)) return JS_NULL;
    return newTA(ctx, "Uint8Array", out.data(), out.size());
}

static JSValue img_encodeJpegFile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "encodeJpegFile(path, pixels, w, h, channels, quality?)");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    TAView px;
    if (!unpackTA(ctx, argv[1], "pixels", &px)) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    int32_t w, h, c, quality = 90;
    if (JS_ToInt32(ctx, &w, argv[2]) || JS_ToInt32(ctx, &h, argv[3]) || JS_ToInt32(ctx, &c, argv[4])) {
        JS_FreeCString(ctx, path); return JS_EXCEPTION;
    }
    if (argc >= 6 && !JS_IsUndefined(argv[5])) { if (JS_ToInt32(ctx, &quality, argv[5])) { JS_FreeCString(ctx, path); return JS_EXCEPTION; } }
    bool ok = broimage::encode_jpeg_file(resolvePath(ctx, path), px.data, w, h, c, quality);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

static JSValue img_encodeJpeg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "encodeJpeg(pixels, w, h, channels, quality?)");
    TAView px;
    if (!unpackTA(ctx, argv[0], "pixels", &px)) return JS_EXCEPTION;
    int32_t w, h, c, quality = 90;
    if (JS_ToInt32(ctx, &w, argv[1]) || JS_ToInt32(ctx, &h, argv[2]) || JS_ToInt32(ctx, &c, argv[3])) return JS_EXCEPTION;
    if (argc >= 5 && !JS_IsUndefined(argv[4])) { if (JS_ToInt32(ctx, &quality, argv[4])) return JS_EXCEPTION; }
    std::vector<uint8_t> out;
    if (!broimage::encode_jpeg_memory(out, px.data, w, h, c, quality)) return JS_NULL;
    return newTA(ctx, "Uint8Array", out.data(), out.size());
}

// -------------------------------------------------------------------------
// Geometric (HWC u8 + f32 resize, letterbox, pad, crop, flip, rotate)
// -------------------------------------------------------------------------

// resizeU8(dst, src, {srcW,srcH,dstW,dstH,channels,filter,srcStride,dstStride})
static JSValue img_resizeU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "resizeU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "resizeU8: dst/src must be Uint8Array");
    int32_t sw, sh, dw, dh, ch, ss, ds;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0)
        return JS_ThrowRangeError(ctx, "resizeU8: dims/channels must be positive");
    std::string fs; if (!propStr(ctx, argv[2], "filter", &fs)) return JS_EXCEPTION;
    broimage::Filter f; if (!parseFilter(ctx, fs, broimage::Filter::Bilinear, &f)) return JS_EXCEPTION;
    broimage::resize_hwc_u8(src.data, sw, sh, ch, dst.data, dw, dh, f, ss, ds);
    return JS_UNDEFINED;
}

// resizeF32(dst, src, {srcW,srcH,dstW,dstH,channels,filter}) — HWC float32,
// full filter set (the kernel-verb `resample` is bilinear/nearest only).
static JSValue img_resizeF32(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "resizeF32(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "resizeF32: dst/src must be Float32Array");
    int32_t sw, sh, dw, dh, ch;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 1)) return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0)
        return JS_ThrowRangeError(ctx, "resizeF32: dims/channels must be positive");
    std::string fs; if (!propStr(ctx, argv[2], "filter", &fs)) return JS_EXCEPTION;
    broimage::Filter f; if (!parseFilter(ctx, fs, broimage::Filter::Bilinear, &f)) return JS_EXCEPTION;
    broimage::resize_hwc_f32(reinterpret_cast<const float*>(src.data), sw, sh, ch,
                             reinterpret_cast<float*>(dst.data), dw, dh, f);
    return JS_UNDEFINED;
}

// resizeChwF32(dst, src, {srcW,srcH,dstW,dstH,channels,filter}) — planar CHW.
static JSValue img_resizeChwF32(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "resizeChwF32(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "resizeChwF32: dst/src must be Float32Array");
    int32_t sw, sh, dw, dh, ch;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 1)) return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0)
        return JS_ThrowRangeError(ctx, "resizeChwF32: dims/channels must be positive");
    std::string fs; if (!propStr(ctx, argv[2], "filter", &fs)) return JS_EXCEPTION;
    broimage::Filter f; if (!parseFilter(ctx, fs, broimage::Filter::Bilinear, &f)) return JS_EXCEPTION;
    broimage::resize_chw_f32(reinterpret_cast<const float*>(src.data), sw, sh, ch,
                             reinterpret_cast<float*>(dst.data), dw, dh, f);
    return JS_UNDEFINED;
}

// Return {x,y,w,h} content rect produced by a letterbox.
static JSValue makeRect(JSContext* ctx, int x, int y, int w, int h) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "x", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, r, "y", JS_NewInt32(ctx, y));
    JS_SetPropertyStr(ctx, r, "w", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, r, "h", JS_NewInt32(ctx, h));
    return r;
}

// letterboxU8(dst, src, {srcW,srcH,dstW,dstH,channels,pad:[r,g,b,a],filter})
static JSValue img_letterboxU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "letterboxU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "letterboxU8: dst/src must be Uint8Array");
    int32_t sw, sh, dw, dh, ch;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4)) return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0)
        return JS_ThrowRangeError(ctx, "letterboxU8: dims/channels must be positive");
    float pad[4]; const float pdef[4] = {0,0,0,255};
    if (!propFloats(ctx, argv[2], "pad", pad, 4, pdef)) return JS_EXCEPTION;
    std::string fs; if (!propStr(ctx, argv[2], "filter", &fs)) return JS_EXCEPTION;
    broimage::Filter f; if (!parseFilter(ctx, fs, broimage::Filter::Bilinear, &f)) return JS_EXCEPTION;
    int ox = 0, oy = 0, ow = 0, oh = 0;
    broimage::letterbox_hwc_u8(src.data, sw, sh, ch, dst.data, dw, dh,
                               (uint8_t)pad[0], (uint8_t)pad[1], (uint8_t)pad[2], (uint8_t)pad[3],
                               f, &ox, &oy, &ow, &oh);
    return makeRect(ctx, ox, oy, ow, oh);
}

// padU8(dst, src, {srcW,srcH,dstW,dstH,channels,offX,offY,pad:[r,g,b,a],srcStride,dstStride})
static JSValue img_padU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "padU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "padU8: dst/src must be Uint8Array");
    int32_t sw, sh, dw, dh, ch, ox, oy, ss, ds;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) ||
        !propI32(ctx, argv[2], "offX", &ox, 0) || !propI32(ctx, argv[2], "offY", &oy, 0) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0)
        return JS_ThrowRangeError(ctx, "padU8: dims/channels must be positive");
    float pad[4]; const float pdef[4] = {0,0,0,255};
    if (!propFloats(ctx, argv[2], "pad", pad, 4, pdef)) return JS_EXCEPTION;
    broimage::pad_hwc_u8(src.data, sw, sh, ch, dst.data, dw, dh, ox, oy,
                         (uint8_t)pad[0], (uint8_t)pad[1], (uint8_t)pad[2], (uint8_t)pad[3], ss, ds);
    return JS_UNDEFINED;
}

// cropU8(dst, src, {srcW,srcH,channels,x,y,w,h,srcStride,dstStride})
static JSValue img_cropU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "cropU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "cropU8: dst/src must be Uint8Array");
    int32_t sw, sh, ch, x, y, w, h, ss, ds;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) ||
        !propI32(ctx, argv[2], "x", &x, 0) || !propI32(ctx, argv[2], "y", &y, 0) ||
        !propI32(ctx, argv[2], "w", &w, 0) || !propI32(ctx, argv[2], "h", &h, 0) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || ch <= 0 || w <= 0 || h <= 0)
        return JS_ThrowRangeError(ctx, "cropU8: dims/channels/rect must be positive");
    broimage::crop_hwc_u8(src.data, sw, sh, ch, dst.data, x, y, w, h, ss, ds);
    return JS_UNDEFINED;
}

// centerCropU8(dst, src, {srcW,srcH,channels,cropW,cropH,srcStride,dstStride})
static JSValue img_centerCropU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "centerCropU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "centerCropU8: dst/src must be Uint8Array");
    int32_t sw, sh, ch, cw, chh, ss, ds;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) ||
        !propI32(ctx, argv[2], "cropW", &cw, 0) || !propI32(ctx, argv[2], "cropH", &chh, 0) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || ch <= 0 || cw <= 0 || chh <= 0)
        return JS_ThrowRangeError(ctx, "centerCropU8: dims/channels/crop must be positive");
    broimage::center_crop_hwc_u8(src.data, sw, sh, ch, dst.data, cw, chh, ss, ds);
    return JS_UNDEFINED;
}

// flipHorizontalU8(dst, src, {w,h,channels,srcStride,dstStride})
static JSValue img_flipHorizontalU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "flipHorizontalU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "flipHorizontalU8: dst/src must be Uint8Array");
    int32_t w, h, ch, ss, ds;
    if (!propI32(ctx, argv[2], "w", &w, 0) || !propI32(ctx, argv[2], "h", &h, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (w <= 0 || h <= 0 || ch <= 0) return JS_ThrowRangeError(ctx, "flipHorizontalU8: dims/channels must be positive");
    broimage::flip_horizontal_hwc_u8(src.data, dst.data, w, h, ch, ss, ds);
    return JS_UNDEFINED;
}

// flipVerticalU8(dst, src, {w,h,channels,srcStride,dstStride})
static JSValue img_flipVerticalU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "flipVerticalU8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "flipVerticalU8: dst/src must be Uint8Array");
    int32_t w, h, ch, ss, ds;
    if (!propI32(ctx, argv[2], "w", &w, 0) || !propI32(ctx, argv[2], "h", &h, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (w <= 0 || h <= 0 || ch <= 0) return JS_ThrowRangeError(ctx, "flipVerticalU8: dims/channels must be positive");
    broimage::flip_vertical_hwc_u8(src.data, dst.data, w, h, ch, ss, ds);
    return JS_UNDEFINED;
}

// rotate90U8(dst, src, {srcW,srcH,channels,turns,srcStride,dstStride}) — turns
// = number of 90-CCW turns. dst dims swap for odd turns.
static JSValue img_rotate90U8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "rotate90U8(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "rotate90U8: dst/src must be Uint8Array");
    int32_t sw, sh, ch, turns, ss, ds;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "channels", &ch, 4) || !propI32(ctx, argv[2], "turns", &turns, 1) ||
        !propI32(ctx, argv[2], "srcStride", &ss, 0) || !propI32(ctx, argv[2], "dstStride", &ds, 0))
        return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || ch <= 0) return JS_ThrowRangeError(ctx, "rotate90U8: dims/channels must be positive");
    broimage::rotate_90_hwc_u8(src.data, sw, sh, ch, dst.data, turns, ss, ds);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Alpha
// -------------------------------------------------------------------------

static JSValue img_premultiplyAlpha(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "premultiplyAlpha(dst, src)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "premultiplyAlpha: buffers must be Uint8Array (RGBA8)");
    int n = (int)(src.byte_len / 4);
    if (dst.byte_len < (size_t)n * 4) return JS_ThrowRangeError(ctx, "premultiplyAlpha: dst too small");
    broimage::premultiply_alpha_rgba8(src.data, dst.data, n);
    return JS_UNDEFINED;
}

static JSValue img_unpremultiplyAlpha(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "unpremultiplyAlpha(dst, src)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "unpremultiplyAlpha: buffers must be Uint8Array (RGBA8)");
    int n = (int)(src.byte_len / 4);
    if (dst.byte_len < (size_t)n * 4) return JS_ThrowRangeError(ctx, "unpremultiplyAlpha: dst too small");
    broimage::unpremultiply_alpha_rgba8(src.data, dst.data, n);
    return JS_UNDEFINED;
}

// resizeRgba8Alpha(dst, src, {srcW,srcH,dstW,dstH,filter}) — alpha-aware resize.
static JSValue img_resizeRgba8Alpha(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "resizeRgba8Alpha(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "resizeRgba8Alpha: dst/src must be Uint8Array");
    int32_t sw, sh, dw, dh;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0)) return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return JS_ThrowRangeError(ctx, "resizeRgba8Alpha: dims must be positive");
    std::string fs; if (!propStr(ctx, argv[2], "filter", &fs)) return JS_EXCEPTION;
    broimage::Filter f; if (!parseFilter(ctx, fs, broimage::Filter::Bilinear, &f)) return JS_EXCEPTION;
    broimage::resize_rgba8_alpha(src.data, sw, sh, dst.data, dw, dh, f);
    return JS_UNDEFINED;
}

// letterboxRgba8Alpha(dst, src, {srcW,srcH,dstW,dstH,pad:[r,g,b,a],filter})
static JSValue img_letterboxRgba8Alpha(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "letterboxRgba8Alpha(dst, src, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "letterboxRgba8Alpha: dst/src must be Uint8Array");
    int32_t sw, sh, dw, dh;
    if (!propI32(ctx, argv[2], "srcW", &sw, 0) || !propI32(ctx, argv[2], "srcH", &sh, 0) ||
        !propI32(ctx, argv[2], "dstW", &dw, 0) || !propI32(ctx, argv[2], "dstH", &dh, 0)) return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return JS_ThrowRangeError(ctx, "letterboxRgba8Alpha: dims must be positive");
    float pad[4]; const float pdef[4] = {0,0,0,0};
    if (!propFloats(ctx, argv[2], "pad", pad, 4, pdef)) return JS_EXCEPTION;
    std::string fs; if (!propStr(ctx, argv[2], "filter", &fs)) return JS_EXCEPTION;
    broimage::Filter f; if (!parseFilter(ctx, fs, broimage::Filter::Bilinear, &f)) return JS_EXCEPTION;
    int ox = 0, oy = 0, ow = 0, oh = 0;
    broimage::letterbox_rgba8_alpha(src.data, sw, sh, dst.data, dw, dh,
                                    (uint8_t)pad[0], (uint8_t)pad[1], (uint8_t)pad[2], (uint8_t)pad[3],
                                    f, &ox, &oy, &ow, &oh);
    return makeRect(ctx, ox, oy, ow, oh);
}

// -------------------------------------------------------------------------
// Color
// -------------------------------------------------------------------------

// Generic u8 channel-convert helper: pixel_count derived from src length and
// the source channel count.
static JSValue u8ChannelConvert(JSContext* ctx, int argc, JSValueConst* argv,
                                const char* name, int srcCh, int dstCh,
                                void (*fn)(const uint8_t*, uint8_t*, int)) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "%s(dst, src)", name);
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "%s: buffers must be Uint8Array", name);
    int n = (int)(src.byte_len / srcCh);
    if (dst.byte_len < (size_t)n * dstCh) return JS_ThrowRangeError(ctx, "%s: dst too small", name);
    fn(src.data, dst.data, n);
    return JS_UNDEFINED;
}

static JSValue img_rgbaToRgb(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return u8ChannelConvert(ctx, argc, argv, "rgbaToRgb", 4, 3, broimage::rgba_to_rgb_u8);
}
static JSValue img_rgbaToGray(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return u8ChannelConvert(ctx, argc, argv, "rgbaToGray", 4, 1, broimage::rgba_to_gray_u8);
}
static JSValue img_rgbToGray(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return u8ChannelConvert(ctx, argc, argv, "rgbToGray", 3, 1, broimage::rgb_to_gray_u8);
}

// rgbToRgba(dst, src, alpha=255)
static JSValue img_rgbToRgba(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "rgbToRgba(dst, src, alpha=255)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1 || src.bpe != 1) return JS_ThrowTypeError(ctx, "rgbToRgba: buffers must be Uint8Array");
    int32_t alpha = 255;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) { if (JS_ToInt32(ctx, &alpha, argv[2])) return JS_EXCEPTION; }
    int n = (int)(src.byte_len / 3);
    if (dst.byte_len < (size_t)n * 4) return JS_ThrowRangeError(ctx, "rgbToRgba: dst too small");
    broimage::rgb_to_rgba_u8(src.data, dst.data, n, (uint8_t)alpha);
    return JS_UNDEFINED;
}

// hwcToChw(dst, src, {width,height,channels}) / chwToHwc — float32.
static JSValue img_hwcToChw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "hwcToChw(dst, src, {width,height,channels})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "hwcToChw: dst/src must be Float32Array");
    int32_t w, h, c;
    if (!propI32(ctx, argv[2], "width", &w, 0) || !propI32(ctx, argv[2], "height", &h, 0) ||
        !propI32(ctx, argv[2], "channels", &c, 0)) return JS_EXCEPTION;
    if (w <= 0 || h <= 0 || c <= 0) return JS_ThrowRangeError(ctx, "hwcToChw: dims/channels must be positive");
    broimage::hwc_to_chw_f32(reinterpret_cast<const float*>(src.data),
                             reinterpret_cast<float*>(dst.data), w, h, c);
    return JS_UNDEFINED;
}

static JSValue img_chwToHwc(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "chwToHwc(dst, src, {width,height,channels})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "chwToHwc: dst/src must be Float32Array");
    int32_t w, h, c;
    if (!propI32(ctx, argv[2], "width", &w, 0) || !propI32(ctx, argv[2], "height", &h, 0) ||
        !propI32(ctx, argv[2], "channels", &c, 0)) return JS_EXCEPTION;
    if (w <= 0 || h <= 0 || c <= 0) return JS_ThrowRangeError(ctx, "chwToHwc: dims/channels must be positive");
    broimage::chw_to_hwc_f32(reinterpret_cast<const float*>(src.data),
                             reinterpret_cast<float*>(dst.data), w, h, c);
    return JS_UNDEFINED;
}

// Generic elementwise float32 op over the whole buffer (gamma / sRGB curves).
static JSValue f32Unary(JSContext* ctx, int argc, JSValueConst* argv, const char* name,
                        void (*fn)(const float*, float*, int)) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "%s(dst, src)", name);
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "%s: dst/src must be Float32Array", name);
    int n = (int)(src.byte_len / 4);
    if (dst.byte_len < (size_t)n * 4) return JS_ThrowRangeError(ctx, "%s: dst too small", name);
    fn(reinterpret_cast<const float*>(src.data), reinterpret_cast<float*>(dst.data), n);
    return JS_UNDEFINED;
}

static JSValue img_applyGamma(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "applyGamma(dst, src, gamma)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "applyGamma: dst/src must be Float32Array");
    double gamma = 1.0;
    if (JS_ToFloat64(ctx, &gamma, argv[2])) return JS_EXCEPTION;
    int n = (int)(src.byte_len / 4);
    if (dst.byte_len < (size_t)n * 4) return JS_ThrowRangeError(ctx, "applyGamma: dst too small");
    broimage::apply_gamma_f32(reinterpret_cast<const float*>(src.data),
                              reinterpret_cast<float*>(dst.data), n, (float)gamma);
    return JS_UNDEFINED;
}

static JSValue img_srgbToLinear(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) {
    return f32Unary(ctx, argc, argv, "srgbToLinear", broimage::srgb_to_linear_f32);
}
static JSValue img_linearToSrgb(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) {
    return f32Unary(ctx, argc, argv, "linearToSrgb", broimage::linear_to_srgb_f32);
}

// srgbToLinearU8ToF32(dstF32, srcU8) — decode + linearize in one pass.
static JSValue img_srgbToLinearU8ToF32(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "srgbToLinearU8ToF32(dst, src)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4) return JS_ThrowTypeError(ctx, "srgbToLinearU8ToF32: dst must be Float32Array");
    if (src.bpe != 1) return JS_ThrowTypeError(ctx, "srgbToLinearU8ToF32: src must be Uint8Array");
    int n = (int)src.byte_len;
    if (dst.byte_len < (size_t)n * 4) return JS_ThrowRangeError(ctx, "srgbToLinearU8ToF32: dst too small");
    broimage::srgb_to_linear_u8_to_f32(src.data, reinterpret_cast<float*>(dst.data), n);
    return JS_UNDEFINED;
}

// linearF32ToSrgbU8(dstU8, srcF32) — encode + gamma-compress in one pass.
static JSValue img_linearF32ToSrgbU8(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "linearF32ToSrgbU8(dst, src)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1) return JS_ThrowTypeError(ctx, "linearF32ToSrgbU8: dst must be Uint8Array");
    if (src.bpe != 4) return JS_ThrowTypeError(ctx, "linearF32ToSrgbU8: src must be Float32Array");
    int n = (int)(src.byte_len / 4);
    if (dst.byte_len < (size_t)n) return JS_ThrowRangeError(ctx, "linearF32ToSrgbU8: dst too small");
    broimage::linear_f32_to_srgb_u8(reinterpret_cast<const float*>(src.data), dst.data, n);
    return JS_UNDEFINED;
}

// HSV/HSL conversions — float32, 3 components per pixel in [0,1].
static JSValue f32PixelTriple(JSContext* ctx, int argc, JSValueConst* argv, const char* name,
                              void (*fn)(const float*, float*, int)) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "%s(dst, src)", name);
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "%s: dst/src must be Float32Array", name);
    int n = (int)(src.byte_len / 4 / 3);
    if (dst.byte_len < (size_t)n * 3 * 4) return JS_ThrowRangeError(ctx, "%s: dst too small", name);
    fn(reinterpret_cast<const float*>(src.data), reinterpret_cast<float*>(dst.data), n);
    return JS_UNDEFINED;
}

static JSValue img_rgbToHsv(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) {
    return f32PixelTriple(ctx, argc, argv, "rgbToHsv", broimage::rgb_to_hsv_f32);
}
static JSValue img_hsvToRgb(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) {
    return f32PixelTriple(ctx, argc, argv, "hsvToRgb", broimage::hsv_to_rgb_f32);
}
static JSValue img_rgbToHsl(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) {
    return f32PixelTriple(ctx, argc, argv, "rgbToHsl", broimage::rgb_to_hsl_f32);
}
static JSValue img_hslToRgb(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) {
    return f32PixelTriple(ctx, argc, argv, "hslToRgb", broimage::hsl_to_rgb_f32);
}

// applyColorMatrix3x3(dst, src, {channels, matrix:[9]}) — matrix row-major.
static JSValue img_applyColorMatrix3x3(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "applyColorMatrix3x3(dst, src, {channels, matrix})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "applyColorMatrix3x3: dst/src must be Float32Array");
    int32_t ch;
    if (!propI32(ctx, argv[2], "channels", &ch, 3)) return JS_EXCEPTION;
    if (ch != 3 && ch != 4) return JS_ThrowRangeError(ctx, "applyColorMatrix3x3: channels must be 3 or 4");
    float m[9];
    if (!propFloats(ctx, argv[2], "matrix", m, 9, nullptr))
        return JS_ThrowTypeError(ctx, "applyColorMatrix3x3: matrix must be 9 numbers");
    int n = (int)(src.byte_len / 4 / ch);
    if (dst.byte_len < (size_t)n * ch * 4) return JS_ThrowRangeError(ctx, "applyColorMatrix3x3: dst too small");
    broimage::apply_color_matrix_3x3_f32(reinterpret_cast<const float*>(src.data),
                                         reinterpret_cast<float*>(dst.data), n, ch, m);
    return JS_UNDEFINED;
}

// applyColorMatrix3x4(dst, src, {channels, matrix:[12]}) — last column is bias.
static JSValue img_applyColorMatrix3x4(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "applyColorMatrix3x4(dst, src, {channels, matrix})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "applyColorMatrix3x4: dst/src must be Float32Array");
    int32_t ch;
    if (!propI32(ctx, argv[2], "channels", &ch, 3)) return JS_EXCEPTION;
    if (ch != 3 && ch != 4) return JS_ThrowRangeError(ctx, "applyColorMatrix3x4: channels must be 3 or 4");
    float m[12];
    if (!propFloats(ctx, argv[2], "matrix", m, 12, nullptr))
        return JS_ThrowTypeError(ctx, "applyColorMatrix3x4: matrix must be 12 numbers");
    int n = (int)(src.byte_len / 4 / ch);
    if (dst.byte_len < (size_t)n * ch * 4) return JS_ThrowRangeError(ctx, "applyColorMatrix3x4: dst too small");
    broimage::apply_color_matrix_3x4_f32(reinterpret_cast<const float*>(src.data),
                                         reinterpret_cast<float*>(dst.data), n, ch, m);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Preproc (NHWC<->NCHW layout shuffles + dtype scale/bias)
// -------------------------------------------------------------------------

// u8NhwcToF32Nchw(dst, src, {N,H,W,C,scale,bias})
static JSValue img_u8NhwcToF32Nchw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "u8NhwcToF32Nchw(dst, src, {N,H,W,C,scale,bias})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4) return JS_ThrowTypeError(ctx, "u8NhwcToF32Nchw: dst must be Float32Array");
    if (src.bpe != 1) return JS_ThrowTypeError(ctx, "u8NhwcToF32Nchw: src must be Uint8Array");
    int32_t N, H, W, C;
    double scale, bias;
    if (!propI32(ctx, argv[2], "N", &N, 1) || !propI32(ctx, argv[2], "H", &H, 0) ||
        !propI32(ctx, argv[2], "W", &W, 0) || !propI32(ctx, argv[2], "C", &C, 0) ||
        !propF64(ctx, argv[2], "scale", &scale, 1.0) || !propF64(ctx, argv[2], "bias", &bias, 0.0))
        return JS_EXCEPTION;
    if (N <= 0 || H <= 0 || W <= 0 || C <= 0) return JS_ThrowRangeError(ctx, "u8NhwcToF32Nchw: N/H/W/C must be positive");
    size_t need = (size_t)N * C * H * W;
    if (dst.byte_len < need * 4) return JS_ThrowRangeError(ctx, "u8NhwcToF32Nchw: dst too small");
    broimage::u8_nhwc_to_f32_nchw(src.data, N, H, W, C, (float)scale, (float)bias,
                                  reinterpret_cast<float*>(dst.data));
    return JS_UNDEFINED;
}

// nhwcToNchwF32(dst, src, {N,H,W,C})
static JSValue img_nhwcToNchwF32(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "nhwcToNchwF32(dst, src, {N,H,W,C})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "nhwcToNchwF32: dst/src must be Float32Array");
    int32_t N, H, W, C;
    if (!propI32(ctx, argv[2], "N", &N, 1) || !propI32(ctx, argv[2], "H", &H, 0) ||
        !propI32(ctx, argv[2], "W", &W, 0) || !propI32(ctx, argv[2], "C", &C, 0)) return JS_EXCEPTION;
    if (N <= 0 || H <= 0 || W <= 0 || C <= 0) return JS_ThrowRangeError(ctx, "nhwcToNchwF32: N/H/W/C must be positive");
    if (dst.byte_len < (size_t)N * C * H * W * 4) return JS_ThrowRangeError(ctx, "nhwcToNchwF32: dst too small");
    broimage::nhwc_to_nchw_f32(reinterpret_cast<const float*>(src.data), N, H, W, C,
                               reinterpret_cast<float*>(dst.data));
    return JS_UNDEFINED;
}

// nchwToNhwcF32(dst, src, {N,C,H,W})
static JSValue img_nchwToNhwcF32(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "nchwToNhwcF32(dst, src, {N,C,H,W})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "nchwToNhwcF32: dst/src must be Float32Array");
    int32_t N, C, H, W;
    if (!propI32(ctx, argv[2], "N", &N, 1) || !propI32(ctx, argv[2], "C", &C, 0) ||
        !propI32(ctx, argv[2], "H", &H, 0) || !propI32(ctx, argv[2], "W", &W, 0)) return JS_EXCEPTION;
    if (N <= 0 || C <= 0 || H <= 0 || W <= 0) return JS_ThrowRangeError(ctx, "nchwToNhwcF32: N/C/H/W must be positive");
    if (dst.byte_len < (size_t)N * H * W * C * 4) return JS_ThrowRangeError(ctx, "nchwToNhwcF32: dst too small");
    broimage::nchw_to_nhwc_f32(reinterpret_cast<const float*>(src.data), N, C, H, W,
                               reinterpret_cast<float*>(dst.data));
    return JS_UNDEFINED;
}

// f32NchwToU8Nhwc(dst, src, {N,C,H,W,scale,bias})
static JSValue img_f32NchwToU8Nhwc(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "f32NchwToU8Nhwc(dst, src, {N,C,H,W,scale,bias})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 1) return JS_ThrowTypeError(ctx, "f32NchwToU8Nhwc: dst must be Uint8Array");
    if (src.bpe != 4) return JS_ThrowTypeError(ctx, "f32NchwToU8Nhwc: src must be Float32Array");
    int32_t N, C, H, W;
    double scale, bias;
    if (!propI32(ctx, argv[2], "N", &N, 1) || !propI32(ctx, argv[2], "C", &C, 0) ||
        !propI32(ctx, argv[2], "H", &H, 0) || !propI32(ctx, argv[2], "W", &W, 0) ||
        !propF64(ctx, argv[2], "scale", &scale, 1.0) || !propF64(ctx, argv[2], "bias", &bias, 0.0))
        return JS_EXCEPTION;
    if (N <= 0 || C <= 0 || H <= 0 || W <= 0) return JS_ThrowRangeError(ctx, "f32NchwToU8Nhwc: N/C/H/W must be positive");
    if (dst.byte_len < (size_t)N * H * W * C) return JS_ThrowRangeError(ctx, "f32NchwToU8Nhwc: dst too small");
    broimage::f32_nchw_to_u8_nhwc(reinterpret_cast<const float*>(src.data), N, C, H, W,
                                  (float)scale, (float)bias, dst.data);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Normalize (per-channel (x-mean)/std on NCHW float32)
// -------------------------------------------------------------------------

// normalizeNchw(dst, src, {N,C,H,W, mean:[C], std:[C]})
static JSValue img_normalizeNchw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "normalizeNchw(dst, src, {N,C,H,W,mean,std})");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "normalizeNchw: dst/src must be Float32Array");
    int32_t N, C, H, W;
    if (!propI32(ctx, argv[2], "N", &N, 1) || !propI32(ctx, argv[2], "C", &C, 0) ||
        !propI32(ctx, argv[2], "H", &H, 0) || !propI32(ctx, argv[2], "W", &W, 0)) return JS_EXCEPTION;
    if (N <= 0 || C <= 0 || H <= 0 || W <= 0) return JS_ThrowRangeError(ctx, "normalizeNchw: N/C/H/W must be positive");
    std::vector<float> mean(C), std_(C);
    if (!propFloats(ctx, argv[2], "mean", mean.data(), C, nullptr))
        return JS_ThrowTypeError(ctx, "normalizeNchw: mean must be an array of length C");
    if (!propFloats(ctx, argv[2], "std", std_.data(), C, nullptr))
        return JS_ThrowTypeError(ctx, "normalizeNchw: std must be an array of length C");
    if (src.byte_len < (size_t)N * C * H * W * 4 || dst.byte_len < (size_t)N * C * H * W * 4)
        return JS_ThrowRangeError(ctx, "normalizeNchw: buffers too small for N*C*H*W");
    broimage::image_normalize_nchw_f32(reinterpret_cast<const float*>(src.data),
                                       mean.data(), std_.data(), N, C, H, W,
                                       reinterpret_cast<float*>(dst.data));
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Multi-channel stencil (HWC float32 convolution, same kernel per channel)
// -------------------------------------------------------------------------

// stencilHwc(dst, src, kernel, {srcW,srcH,channels,edge,divisor,bias})
static JSValue img_stencilHwc(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "stencilHwc(dst, src, kernel, params)");
    TAView dst, src;
    if (!unpackTA(ctx, argv[0], "dst", &dst)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "src", &src)) return JS_EXCEPTION;
    if (dst.bpe != 4 || src.bpe != 4) return JS_ThrowTypeError(ctx, "stencilHwc: dst/src must be Float32Array");

    JSValue kdata_v = JS_GetPropertyStr(ctx, argv[2], "data");
    int32_t kw, kh;
    if (!propI32(ctx, argv[2], "w", &kw, 0) || !propI32(ctx, argv[2], "h", &kh, 0)) {
        JS_FreeValue(ctx, kdata_v); return JS_EXCEPTION;
    }
    TAView kdata;
    if (!unpackTA(ctx, kdata_v, "kernel.data", &kdata)) { JS_FreeValue(ctx, kdata_v); return JS_EXCEPTION; }
    JS_FreeValue(ctx, kdata_v);
    if (kdata.bpe != 4) return JS_ThrowTypeError(ctx, "stencilHwc: kernel.data must be Float32Array");
    if (kw <= 0 || kh <= 0 || (kw & 1) == 0 || (kh & 1) == 0)
        return JS_ThrowRangeError(ctx, "stencilHwc: kernel w/h must be positive and odd");
    if (kdata.byte_len < (size_t)kw * kh * 4) return JS_ThrowRangeError(ctx, "stencilHwc: kernel.data too small");

    int32_t sw, sh, ch;
    if (!propI32(ctx, argv[3], "srcW", &sw, 0) || !propI32(ctx, argv[3], "srcH", &sh, 0) ||
        !propI32(ctx, argv[3], "channels", &ch, 1)) return JS_EXCEPTION;
    if (sw <= 0 || sh <= 0 || ch <= 0) return JS_ThrowRangeError(ctx, "stencilHwc: srcW/srcH/channels must be positive");
    if (src.byte_len < (size_t)sw * sh * ch * 4 || dst.byte_len < (size_t)sw * sh * ch * 4)
        return JS_ThrowRangeError(ctx, "stencilHwc: buffers too small for srcW*srcH*channels");

    std::string edge; if (!propStr(ctx, argv[3], "edge", &edge)) return JS_EXCEPTION;
    broimage::StencilEdge be = broimage::StencilEdge::Clamp;
    if (edge == "wrap") be = broimage::StencilEdge::Wrap;
    else if (edge == "zero") be = broimage::StencilEdge::Zero;
    else if (!edge.empty() && edge != "clamp") return JS_ThrowTypeError(ctx, "stencilHwc: edge must be clamp|wrap|zero");
    double divisor, bias;
    if (!propF64(ctx, argv[3], "divisor", &divisor, 1.0) || !propF64(ctx, argv[3], "bias", &bias, 0.0)) return JS_EXCEPTION;

    broimage::stencil_hwc_f32(reinterpret_cast<const float*>(src.data),
                              reinterpret_cast<float*>(dst.data), sw, sh, ch,
                              reinterpret_cast<const float*>(kdata.data), kw, kh,
                              (float)divisor, (float)bias, be);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Tiling (feather window, weighted accumulate, normalize)
// -------------------------------------------------------------------------

// featherWindow(win, {tw,th,ovL,ovR,ovT,ovB}) — fills single-channel weights.
static JSValue img_featherWindow(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "featherWindow(win, {tw,th,ovL,ovR,ovT,ovB})");
    TAView win;
    if (!unpackTA(ctx, argv[0], "win", &win)) return JS_EXCEPTION;
    if (win.bpe != 4) return JS_ThrowTypeError(ctx, "featherWindow: win must be Float32Array");
    int32_t tw, th, ovL, ovR, ovT, ovB;
    if (!propI32(ctx, argv[1], "tw", &tw, 0) || !propI32(ctx, argv[1], "th", &th, 0) ||
        !propI32(ctx, argv[1], "ovL", &ovL, 0) || !propI32(ctx, argv[1], "ovR", &ovR, 0) ||
        !propI32(ctx, argv[1], "ovT", &ovT, 0) || !propI32(ctx, argv[1], "ovB", &ovB, 0)) return JS_EXCEPTION;
    if (tw <= 0 || th <= 0) return JS_ThrowRangeError(ctx, "featherWindow: tw/th must be positive");
    if (win.byte_len < (size_t)tw * th * 4) return JS_ThrowRangeError(ctx, "featherWindow: win too small for tw*th");
    broimage::feather_window_f32(reinterpret_cast<float*>(win.data), tw, th, ovL, ovR, ovT, ovB);
    return JS_UNDEFINED;
}

// accumulateTile(acc, wacc, tile, window, {fullW,fullH,channels,tw,th,dstX,dstY})
static JSValue img_accumulateTile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "accumulateTile(acc, wacc, tile, window, params)");
    TAView acc, wacc, tile, window;
    if (!unpackTA(ctx, argv[0], "acc", &acc)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "wacc", &wacc)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[2], "tile", &tile)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[3], "window", &window)) return JS_EXCEPTION;
    if (acc.bpe != 4 || wacc.bpe != 4 || tile.bpe != 4 || window.bpe != 4)
        return JS_ThrowTypeError(ctx, "accumulateTile: all buffers must be Float32Array");
    int32_t fw, fh, ch, tw, th, dx, dy;
    if (!propI32(ctx, argv[4], "fullW", &fw, 0) || !propI32(ctx, argv[4], "fullH", &fh, 0) ||
        !propI32(ctx, argv[4], "channels", &ch, 1) ||
        !propI32(ctx, argv[4], "tw", &tw, 0) || !propI32(ctx, argv[4], "th", &th, 0) ||
        !propI32(ctx, argv[4], "dstX", &dx, 0) || !propI32(ctx, argv[4], "dstY", &dy, 0)) return JS_EXCEPTION;
    if (fw <= 0 || fh <= 0 || ch <= 0 || tw <= 0 || th <= 0)
        return JS_ThrowRangeError(ctx, "accumulateTile: dims/channels must be positive");
    if (acc.byte_len < (size_t)fw * fh * ch * 4 || wacc.byte_len < (size_t)fw * fh * 4)
        return JS_ThrowRangeError(ctx, "accumulateTile: acc/wacc too small for fullW*fullH");
    if (tile.byte_len < (size_t)tw * th * ch * 4 || window.byte_len < (size_t)tw * th * 4)
        return JS_ThrowRangeError(ctx, "accumulateTile: tile/window too small for tw*th");
    broimage::accumulate_tile_f32(reinterpret_cast<float*>(acc.data),
                                  reinterpret_cast<float*>(wacc.data), fw, fh, ch,
                                  reinterpret_cast<const float*>(tile.data), tw, th, dx, dy,
                                  reinterpret_cast<const float*>(window.data));
    return JS_UNDEFINED;
}

// normalizeAccumulator(acc, wacc, {nPixels,channels,eps}) — resolve in place.
static JSValue img_normalizeAccumulator(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "normalizeAccumulator(acc, wacc, {nPixels,channels,eps})");
    TAView acc, wacc;
    if (!unpackTA(ctx, argv[0], "acc", &acc)) return JS_EXCEPTION;
    if (!unpackTA(ctx, argv[1], "wacc", &wacc)) return JS_EXCEPTION;
    if (acc.bpe != 4 || wacc.bpe != 4) return JS_ThrowTypeError(ctx, "normalizeAccumulator: acc/wacc must be Float32Array");
    int32_t np, ch;
    double eps;
    if (!propI32(ctx, argv[2], "nPixels", &np, 0) || !propI32(ctx, argv[2], "channels", &ch, 1) ||
        !propF64(ctx, argv[2], "eps", &eps, 1e-6)) return JS_EXCEPTION;
    if (np <= 0 || ch <= 0) return JS_ThrowRangeError(ctx, "normalizeAccumulator: nPixels/channels must be positive");
    if (acc.byte_len < (size_t)np * ch * 4 || wacc.byte_len < (size_t)np * 4)
        return JS_ThrowRangeError(ctx, "normalizeAccumulator: buffers too small for nPixels");
    broimage::normalize_accumulator_f32(reinterpret_cast<float*>(acc.data),
                                        reinterpret_cast<const float*>(wacc.data), np, ch, (float)eps);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Registration: augment the brokit-built bro.image with broimage's rest.
// -------------------------------------------------------------------------

static void setFn(JSContext* ctx, JSValue obj, const char* name, JSCFunction* fn, int len) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, len));
}

// Add a {mean, std} preset entry as plain JS arrays.
static void setPreset(JSContext* ctx, JSValue presets, const char* name,
                      const float* mean, const float* std_) {
    JSValue e = JS_NewObject(ctx);
    JSValue m = JS_NewArray(ctx), s = JS_NewArray(ctx);
    for (int i = 0; i < 3; i++) {
        JS_SetPropertyUint32(ctx, m, i, JS_NewFloat64(ctx, mean[i]));
        JS_SetPropertyUint32(ctx, s, i, JS_NewFloat64(ctx, std_[i]));
    }
    JS_SetPropertyStr(ctx, e, "mean", m);
    JS_SetPropertyStr(ctx, e, "std", s);
    JS_SetPropertyStr(ctx, presets, name, e);
}

static void registerImageKernels(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue bro = JS_GetPropertyStr(ctx, global, "bro");
    if (!JS_IsObject(bro)) {
        JS_FreeValue(ctx, bro);
        bro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, bro));
    }
    JSValue image = JS_GetPropertyStr(ctx, bro, "image");
    if (!JS_IsObject(image)) {
        JS_FreeValue(ctx, image);
        image = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, bro, "image", JS_DupValue(ctx, image));
    }

    // Decode / probe / EXIF
    setFn(ctx, image, "decodeU16", img_decodeU16, 1);
    setFn(ctx, image, "decodeF32", img_decodeF32, 1);
    setFn(ctx, image, "decodeOriented", img_decodeOriented, 1);
    setFn(ctx, image, "probeDimensions", img_probeDimensions, 1);
    setFn(ctx, image, "readExifOrientation", img_readExifOrientation, 1);
    setFn(ctx, image, "applyExifOrientation", img_applyExifOrientation, 4);

    // Encode
    setFn(ctx, image, "encodePngFile", img_encodePngFile, 5);
    setFn(ctx, image, "encodePng", img_encodePng, 4);
    setFn(ctx, image, "encodeJpegFile", img_encodeJpegFile, 5);
    setFn(ctx, image, "encodeJpeg", img_encodeJpeg, 4);

    // Geometric
    setFn(ctx, image, "resizeU8", img_resizeU8, 3);
    setFn(ctx, image, "resizeF32", img_resizeF32, 3);
    setFn(ctx, image, "resizeChwF32", img_resizeChwF32, 3);
    setFn(ctx, image, "letterboxU8", img_letterboxU8, 3);
    setFn(ctx, image, "padU8", img_padU8, 3);
    setFn(ctx, image, "cropU8", img_cropU8, 3);
    setFn(ctx, image, "centerCropU8", img_centerCropU8, 3);
    setFn(ctx, image, "flipHorizontalU8", img_flipHorizontalU8, 3);
    setFn(ctx, image, "flipVerticalU8", img_flipVerticalU8, 3);
    setFn(ctx, image, "rotate90U8", img_rotate90U8, 3);

    // Alpha
    setFn(ctx, image, "premultiplyAlpha", img_premultiplyAlpha, 2);
    setFn(ctx, image, "unpremultiplyAlpha", img_unpremultiplyAlpha, 2);
    setFn(ctx, image, "resizeRgba8Alpha", img_resizeRgba8Alpha, 3);
    setFn(ctx, image, "letterboxRgba8Alpha", img_letterboxRgba8Alpha, 3);

    // Color
    setFn(ctx, image, "rgbaToRgb", img_rgbaToRgb, 2);
    setFn(ctx, image, "rgbToRgba", img_rgbToRgba, 3);
    setFn(ctx, image, "rgbaToGray", img_rgbaToGray, 2);
    setFn(ctx, image, "rgbToGray", img_rgbToGray, 2);
    setFn(ctx, image, "hwcToChw", img_hwcToChw, 3);
    setFn(ctx, image, "chwToHwc", img_chwToHwc, 3);
    setFn(ctx, image, "applyGamma", img_applyGamma, 3);
    setFn(ctx, image, "srgbToLinear", img_srgbToLinear, 2);
    setFn(ctx, image, "linearToSrgb", img_linearToSrgb, 2);
    setFn(ctx, image, "srgbToLinearU8ToF32", img_srgbToLinearU8ToF32, 2);
    setFn(ctx, image, "linearF32ToSrgbU8", img_linearF32ToSrgbU8, 2);
    setFn(ctx, image, "rgbToHsv", img_rgbToHsv, 2);
    setFn(ctx, image, "hsvToRgb", img_hsvToRgb, 2);
    setFn(ctx, image, "rgbToHsl", img_rgbToHsl, 2);
    setFn(ctx, image, "hslToRgb", img_hslToRgb, 2);
    setFn(ctx, image, "applyColorMatrix3x3", img_applyColorMatrix3x3, 3);
    setFn(ctx, image, "applyColorMatrix3x4", img_applyColorMatrix3x4, 3);

    // Preproc
    setFn(ctx, image, "u8NhwcToF32Nchw", img_u8NhwcToF32Nchw, 3);
    setFn(ctx, image, "nhwcToNchwF32", img_nhwcToNchwF32, 3);
    setFn(ctx, image, "nchwToNhwcF32", img_nchwToNhwcF32, 3);
    setFn(ctx, image, "f32NchwToU8Nhwc", img_f32NchwToU8Nhwc, 3);

    // Normalize + presets
    setFn(ctx, image, "normalizeNchw", img_normalizeNchw, 3);
    {
        JSValue presets = JS_NewObject(ctx);
        setPreset(ctx, presets, "clip", broimage::CLIP_MEAN, broimage::CLIP_STD);
        setPreset(ctx, presets, "imagenet", broimage::IMAGENET_MEAN, broimage::IMAGENET_STD);
        setPreset(ctx, presets, "sam", broimage::SAM_MEAN, broimage::SAM_STD);
        JS_SetPropertyStr(ctx, image, "presets", presets);
    }

    // Multi-channel stencil
    setFn(ctx, image, "stencilHwc", img_stencilHwc, 4);

    // Tiling
    setFn(ctx, image, "featherWindow", img_featherWindow, 2);
    setFn(ctx, image, "accumulateTile", img_accumulateTile, 5);
    setFn(ctx, image, "normalizeAccumulator", img_normalizeAccumulator, 3);

    JS_FreeValue(ctx, image);
    JS_FreeValue(ctx, bro);
    JS_FreeValue(ctx, global);
}

} // anonymous namespace

// -------------------------------------------------------------------------
// Install
// -------------------------------------------------------------------------

void ImageBindings::install(JSContext* ctx, const std::string& basePath,
                            const util::AssetMounts* mounts) {
    s_bases[ctx] = AssetBase{ basePath, mounts };
    s_fallbackBase = AssetBase{ basePath, mounts };

    qjsbind::Class<ID>(ctx, "Image")
        .constructor([](JSContext* ctx, int /*argc*/, JSValueConst* /*argv*/) -> ID* {
            auto* img = new ID();
            img->ctx = ctx;
            return img;
        })
        .get("width", [](ID* self) -> int { return self->width; })
        .get("height", [](ID* self) -> int { return self->height; })
        .get("naturalWidth", [](ID* self) -> int { return self->width; })
        .get("naturalHeight", [](ID* self) -> int { return self->height; })
        .get("complete", [](ID* self) -> bool { return self->complete; })
        .get("src", [](ID* self) -> std::string { return self->src; })
        // src setter is complex (decode + onload callback) — use prop with raw setter
        // We can't use .prop() with a raw setter, so register src getter above and
        // override with DefinePropertyGetSet below after the chain.
        .get("onload", [](ID* self, JSContext* ctx) -> JSValue {
            return JS_DupValue(ctx, self->onload);
        })
        .method_raw("addEventListener", js_image_addEventListener, 2)
        .method_raw("removeEventListener", js_image_removeEventListener, 2)
        .gc_mark([](ID* img, JSRuntime* rt, JS_MarkFunc* mark) {
            JS_MarkValue(rt, img->onload, mark);
            JS_MarkValue(rt, img->onerror, mark);
        });

    // Manually set up src and onload as read-write properties with raw setters.
    // We need to override the read-only getters set above with proper get+set pairs.
    JSValue proto = JS_GetClassProto(ctx, qjsbind::class_id<ID>());

    // src property: getter (returns string) + raw setter (loads image)
    {
        JSAtom atom = JS_NewAtom(ctx, "src");
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* img = qjsbind::unwrap<ID>(ctx, this_val);
                if (!img) return JS_UNDEFINED;
                return JS_NewString(ctx, img->src.c_str());
            }, "src", 0),
            JS_NewCFunction(ctx, js_image_set_src, "src", 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }

    // onload property: getter (returns dup'd JSValue) + raw setter (ref-counted)
    {
        JSAtom atom = JS_NewAtom(ctx, "onload");
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* img = qjsbind::unwrap<ID>(ctx, this_val);
                if (!img) return JS_UNDEFINED;
                return JS_DupValue(ctx, img->onload);
            }, "onload", 0),
            JS_NewCFunction(ctx, js_image_set_onload, "onload", 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }

    // onerror property: fires when the decode fails (missing/corrupt asset)
    {
        JSAtom atom = JS_NewAtom(ctx, "onerror");
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* img = qjsbind::unwrap<ID>(ctx, this_val);
                if (!img) return JS_UNDEFINED;
                return JS_DupValue(ctx, img->onerror);
            }, "onerror", 0),
            JS_NewCFunction(ctx, js_image_set_onerror, "onerror", 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }

    JS_FreeValue(ctx, proto);

    // Sit Image.prototype on HTMLImageElement.prototype rather than aliasing the
    // two constructors together (which would replace the interface object
    // installHtmlInterfaces put on the global, and leave a DOM <img> failing
    // `instanceof HTMLImageElement`).
    //
    // `new Image()` builds this decode helper, not a DOM element, but the web
    // says the result is an HTMLImageElement and libraries test for it: three.js
    // asks `image instanceof HTMLImageElement` to decide whether a texture can
    // be serialized, and answers "no" by dropping it. Chaining the prototypes
    // makes both spellings of an image — the helper and a real <img> — pass the
    // same guard.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue htmlImageCtor = JS_GetPropertyStr(ctx, global, "HTMLImageElement");
    if (JS_IsFunction(ctx, htmlImageCtor)) {
        JSValue htmlImageProto = JS_GetPropertyStr(ctx, htmlImageCtor, "prototype");
        JSValue imageCtor = JS_GetPropertyStr(ctx, global, "Image");
        JSValue imageProto = JS_GetPropertyStr(ctx, imageCtor, "prototype");
        if (JS_IsObject(htmlImageProto) && JS_IsObject(imageProto)) {
            JS_SetPrototype(ctx, imageProto, htmlImageProto);
        }
        JS_FreeValue(ctx, imageProto);
        JS_FreeValue(ctx, imageCtor);
        JS_FreeValue(ctx, htmlImageProto);
    } else {
        // No interface objects in this realm (a worker, say) — keep the old
        // alias so HTMLImageElement is at least defined.
        JSValue imageCtor = JS_GetPropertyStr(ctx, global, "Image");
        JS_SetPropertyStr(ctx, global, "HTMLImageElement", JS_DupValue(ctx, imageCtor));
        JS_FreeValue(ctx, imageCtor);
    }
    JS_FreeValue(ctx, htmlImageCtor);
    JS_FreeValue(ctx, global);

    // Augment the brokit-built bro.image with the rest of broimage's surface
    // (decode/encode, geometric, alpha, color, preproc, normalize, multi-channel
    // stencil, tiling). brokit::api::installAll() ran earlier and created
    // bro.image with the core verb kernels; this adds onto the same object.
    registerImageKernels(ctx);
}

void ImageBindings::cleanup(JSContext* ctx) {
    s_bases.erase(ctx);
}

void ImageBindings::installKernels(JSContext* ctx) {
    registerImageKernels(ctx);
}

JSValue ImageBindings::createImage(JSContext* ctx) {
    auto* img = new ID();
    img->ctx = ctx;
    return qjsbind::wrap<ID>(ctx, img);
}

// A decoded <img> element's pixels, kept keyed on its `src` so re-uploading
// the same texture every frame does not re-decode the file every frame.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool ImageBindings::getImagePixels(JSValue val, ImagePixels& out) {
    // 1) Loaded Image (PNG/JPG decoded via broimage, or 1x1 fallback).
    if (auto* img = qjsbind::unwrap<ID>(nullptr, val)) {
        if (img->complete && !img->pixels.empty()) {
            out.data   = img->pixels.data();
            out.width  = img->width;
            out.height = img->height;
            return true;
        }
    }

    // 2) HTMLCanvasElement — snapshot its 2D context's surface. The Canvas 2D
    //    spec lets any CanvasImageSource feed drawImage / WebGL texImage2D,
    //    and other canvases are a load-bearing CanvasImageSource (offscreen
    //    sprite atlases, recipe-baked tilesets, etc.). The scene caches the
    //    snapshot until the next mutation, so atlas-style usage (one bake,
    //    many blits) doesn't pay a GPU readback per draw.
    if (auto* el = getElement(val)) {
        auto* scene = static_cast<bro::canvas::CanvasScene*>(el->canvasScene());
        if (scene) {
            int w = std::atoi(el->getAttribute("width").c_str());
            int h = std::atoi(el->getAttribute("height").c_str());
            if (w <= 0) w = 300;   // HTML canvas defaults
            if (h <= 0) h = 150;
            const uint8_t* px = scene->snapshotPixels(w, h);
            if (px) {
                out.data   = px;
                out.width  = w;
                out.height = h;
                // A cache hit touches no GL at all, but the miss runs Ganesh on
                // the shared context and leaves its state behind. Flag both: the
                // caller pays one restoreState() on a path that just did a GPU
                // readback, and guessing wrong the other way corrupts the frame.
                out.disturbedGlState = true;
                return true;
            }
        }
    }

    // 2b) A DOM <img> element. document.createElement('img') builds one of
    //     these, so anything a page draws to a canvas or uploads as a texture
    //     after creating the image the ordinary way arrives here. The bytes are
    //     read and decoded on demand and cached on the element's src, because a
    //     texture atlas re-uploaded per frame must not re-decode a PNG per
    //     frame. SVG goes through the rasterizer, so a vector icon is as usable
    //     a source as a bitmap one.
    if (auto* el = getElement(val)) {
        const std::string& tag = el->tagName();
        if (tag == "IMG" || tag == "img") {
            const std::string src = el->getAttribute("src");
            if (!src.empty()) {
                // An <img>'s src resolves against its own document, not against
                // whichever realm happens to be installed last — which is both
                // what the web says and the only answer available here, since
                // getImagePixels has no JSContext to ask.
                AssetBase base = s_fallbackBase;
                if (auto* doc = el->document(); doc && !doc->basePath().empty()) {
                    base.path = doc->basePath();
                }
                const std::string path = resolveAgainst(base, src);

                // Keyed on the resolved path, not the src: the same
                // "images/icon.png" names different files in an app and in a
                // system panel, and one cache shared by every document must not
                // hand the second one the first one's pixels.
                static std::unordered_map<std::string, DecodedImage> s_cache;
                auto it = s_cache.find(path);
                if (it == s_cache.end()) {
                    DecodedImage entry;
                    broimage::Image decoded;
                    std::string err;
                    if (broimage::decode_file(path, decoded, &err)) {
                        entry.width  = decoded.width;
                        entry.height = decoded.height;
                        entry.pixels = std::move(decoded.pixels);
                    } else {
                        std::string markup;
                        std::ifstream f(path, std::ios::binary);
                        if (f) {
                            std::ostringstream ss;
                            ss << f.rdbuf();
                            markup = ss.str();
                        }
                        if (bro::svg::looksLikeSvg(markup.data(), markup.size())) {
                            bro::svg::rasterizeSvgMarkup(markup.data(), markup.size(),
                                                         0, 0, entry.width, entry.height,
                                                         entry.pixels);
                        }
                    }
                    it = s_cache.emplace(path, std::move(entry)).first;
                }
                if (!it->second.pixels.empty()) {
                    out.data   = it->second.pixels.data();
                    out.width  = it->second.width;
                    out.height = it->second.height;
                    return true;
                }
            }
        }
    }

    // 3) ImageBitmap — immutable raster SkImage. Read its RGBA into the owned
    //    buffer. (drawImage has a faster path that draws the SkImage directly;
    //    this serves the drawImage slow path and WebGL texImage2D.)
    if (sk_sp<SkImage> bmp = ImageBitmapBindings::getImage(val)) {
        int w = bmp->width(), h = bmp->height();
        if (w > 0 && h > 0) {
            SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                                 kUnpremul_SkAlphaType);
            out.owned.resize(static_cast<size_t>(w) * h * 4);
            if (bmp->readPixels(nullptr, info, out.owned.data(),
                                static_cast<size_t>(w) * 4, 0, 0)) {
                out.data   = out.owned.data();
                out.width  = w;
                out.height = h;
                return true;
            }
        }
    }

    return false;
}

} // namespace bro::js

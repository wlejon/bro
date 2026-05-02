#include "js/image_bindings.h"
#include "js/dom_bindings_internal.h"
#include "canvas/canvas_scene.h"

#include <qjsbind/qjsbind.h>

#include "util/asset_mounts.h"
#include "util/log.h"

#include <stb_image.h>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>

namespace bro::js {

static std::string s_basePath;  // App directory for resolving relative paths
static const util::AssetMounts* s_mounts = nullptr;

struct ImageData {
    int width = 0;
    int height = 0;
    std::string src;
    std::vector<uint8_t> pixels; // RGBA
    bool complete = false;
    JSValue onload = JS_UNDEFINED; // stored callback
    JSContext* ctx = nullptr;
};

using ID = ImageData;

// Resolve an image src path against the app base directory and engine mounts.
static std::string resolvePath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;   // Windows C:\...
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (s_mounts) {
            std::string m = s_mounts->resolve(src);
            if (!m.empty()) return m;
        }
        return src;
    }
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

// -------------------------------------------------------------------------
// Complex property setters/methods needing raw signatures
// -------------------------------------------------------------------------

// src setter — loads image via stb_image and fires onload
static JSValue js_image_set_src(JSContext* ctx, JSValueConst this_val,
                                int /*argc*/, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    img->src = s;
    JS_FreeCString(ctx, s);

    // Resolve path and load with stb_image
    std::string path = resolvePath(img->src);
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4); // Force RGBA
    if (pixels) {
        img->width = w;
        img->height = h;
        img->pixels.assign(pixels, pixels + w * h * 4);
        stbi_image_free(pixels);
        img->complete = true;
        LOG_INFO("Image loaded: %s (%dx%d)", img->src.c_str(), w, h);
    } else {
        // Failed to load — use 1x1 white fallback
        LOG_WARN("Image load failed: %s (%s)", path.c_str(), stbi_failure_reason());
        img->width = 1;
        img->height = 1;
        img->pixels = {255, 255, 255, 255};
        img->complete = true;
    }

    // Fire onload callback if set
    if (JS_IsFunction(ctx, img->onload)) {
        JSValue func = JS_DupValue(ctx, img->onload);
        JSValue ret = JS_Call(ctx, func, this_val, 0, nullptr);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, func);
    }
    return JS_UNDEFINED;
}

// onload setter — manages JSValue ref counting
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

// addEventListener — dispatch "load" via onload
static JSValue js_image_addEventListener(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* img = qjsbind::unwrap<ID>(ctx, this_val);
    if (!img || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (std::string(type) == "load") {
        if (!JS_IsUndefined(img->onload)) JS_FreeValue(ctx, img->onload);
        img->onload = JS_DupValue(ctx, argv[1]);
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
    if (std::string(type) == "load") {
        if (!JS_IsUndefined(img->onload)) {
            JS_FreeValue(ctx, img->onload);
            img->onload = JS_UNDEFINED;
        }
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Install
// -------------------------------------------------------------------------

void ImageBindings::install(JSContext* ctx, const std::string& basePath,
                            const util::AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;

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
        // src setter is complex (stb_image load + onload callback) — use prop with raw setter
        // We can't use .prop() with a raw setter, so register src getter above and
        // override with DefinePropertyGetSet below after the chain.
        .get("onload", [](ID* self, JSContext* ctx) -> JSValue {
            return JS_DupValue(ctx, self->onload);
        })
        .method_raw("addEventListener", js_image_addEventListener, 2)
        .method_raw("removeEventListener", js_image_removeEventListener, 2);

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

    JS_FreeValue(ctx, proto);

    // Register HTMLImageElement as an alias for Image on global
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue imageCtor = JS_GetPropertyStr(ctx, global, "Image");
    JS_SetPropertyStr(ctx, global, "HTMLImageElement", JS_DupValue(ctx, imageCtor));
    JS_FreeValue(ctx, imageCtor);
    JS_FreeValue(ctx, global);
}

JSValue ImageBindings::createImage(JSContext* ctx) {
    auto* img = new ID();
    img->ctx = ctx;
    return qjsbind::wrap<ID>(ctx, img);
}

bool ImageBindings::getImagePixels(JSValue val, ImagePixels& out) {
    // 1) Loaded Image (PNG/JPG decoded via stb_image, or 1x1 fallback).
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
                return true;
            }
        }
    }

    return false;
}

} // namespace bro::js

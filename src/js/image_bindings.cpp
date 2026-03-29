#include "js/image_bindings.h"
#include "util/log.h"

#include <quickjs.h>
#include <stb_image.h>
#include <string>
#include <vector>
#include <cstdint>

namespace bro::js {

static JSClassID js_image_class_id = 0;
static std::string s_basePath;  // App directory for resolving relative paths

struct ImageData {
    int width = 0;
    int height = 0;
    std::string src;
    std::vector<uint8_t> pixels; // RGBA
    bool complete = false;
    JSValue onload = JS_UNDEFINED; // stored callback
    JSContext* ctx = nullptr;
};

static void js_image_finalizer(JSRuntime*, JSValue val) {
    auto* img = static_cast<ImageData*>(JS_GetOpaque(val, js_image_class_id));
    if (img) {
        delete img;
    }
}

static JSClassDef js_image_class = {
    "Image", js_image_finalizer, nullptr, nullptr, nullptr
};

static inline ImageData* getImage(JSValueConst val) {
    return static_cast<ImageData*>(JS_GetOpaque(val, js_image_class_id));
}

// Resolve an image src path against the app base directory.
static std::string resolvePath(const std::string& src) {
    // Already absolute?
    if (src.size() >= 2 && src[1] == ':') return src;   // Windows C:\...
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) return src;
    // Relative — join with base
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

// --- Properties ---

static JSValue js_image_get_width(JSContext* ctx, JSValueConst this_val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    return JS_NewInt32(ctx, img->width);
}

static JSValue js_image_get_height(JSContext* ctx, JSValueConst this_val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    return JS_NewInt32(ctx, img->height);
}

static JSValue js_image_get_src(JSContext* ctx, JSValueConst this_val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    return JS_NewString(ctx, img->src.c_str());
}

static JSValue js_image_set_src(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
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
        JSValue ret = JS_Call(ctx, img->onload, this_val, 0, nullptr);
        JS_FreeValue(ctx, ret);
    }
    return JS_UNDEFINED;
}

static JSValue js_image_get_complete(JSContext* ctx, JSValueConst this_val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    return JS_NewBool(ctx, img->complete);
}

static JSValue js_image_get_onload(JSContext* ctx, JSValueConst this_val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    return JS_DupValue(ctx, img->onload);
}

static JSValue js_image_set_onload(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* img = getImage(this_val); if (!img) return JS_UNDEFINED;
    if (!JS_IsUndefined(img->onload)) {
        JS_FreeValue(ctx, img->onload);
    }
    img->onload = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_image_get_naturalWidth(JSContext* ctx, JSValueConst this_val) {
    return js_image_get_width(ctx, this_val);
}

static JSValue js_image_get_naturalHeight(JSContext* ctx, JSValueConst this_val) {
    return js_image_get_height(ctx, this_val);
}

// addEventListener/removeEventListener — dispatch "load" via onload
static JSValue js_image_addEventListener(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* img = getImage(this_val); if (!img || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (std::string(type) == "load") {
        if (!JS_IsUndefined(img->onload)) JS_FreeValue(ctx, img->onload);
        img->onload = JS_DupValue(ctx, argv[1]);
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue js_image_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* img = getImage(this_val); if (!img || argc < 2) return JS_UNDEFINED;
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

static const JSCFunctionListEntry js_image_proto_funcs[] = {
    JS_CGETSET_DEF("width", js_image_get_width, nullptr),
    JS_CGETSET_DEF("height", js_image_get_height, nullptr),
    JS_CGETSET_DEF("naturalWidth", js_image_get_naturalWidth, nullptr),
    JS_CGETSET_DEF("naturalHeight", js_image_get_naturalHeight, nullptr),
    JS_CGETSET_DEF("src", js_image_get_src, js_image_set_src),
    JS_CGETSET_DEF("complete", js_image_get_complete, nullptr),
    JS_CGETSET_DEF("onload", js_image_get_onload, js_image_set_onload),
    JS_CFUNC_DEF("addEventListener", 2, js_image_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, js_image_removeEventListener),
};

// --- Constructor ---

static JSValue js_image_constructor(JSContext* ctx, JSValueConst /*new_target*/,
                                     int /*argc*/, JSValueConst* /*argv*/) {
    JSValue obj = JS_NewObjectClass(ctx, (int)js_image_class_id);
    if (JS_IsException(obj)) return obj;
    auto* img = new ImageData();
    img->ctx = ctx;
    JS_SetOpaque(obj, img);
    return obj;
}

// --- Install ---

void ImageBindings::install(JSContext* ctx, const std::string& basePath) {
    s_basePath = basePath;

    JSRuntime* rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &js_image_class_id);
    JS_NewClass(rt, js_image_class_id, &js_image_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_image_proto_funcs,
                               sizeof(js_image_proto_funcs) / sizeof(js_image_proto_funcs[0]));
    JS_SetClassProto(ctx, js_image_class_id, proto);

    // Register Image constructor on global
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_NewCFunction2(ctx, js_image_constructor, "Image", 0,
                                     JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "Image", ctor);
    JS_SetPropertyStr(ctx, global, "HTMLImageElement", JS_DupValue(ctx, ctor));
    JS_FreeValue(ctx, global);
}

JSValue ImageBindings::createImage(JSContext* ctx) {
    JSValue obj = JS_NewObjectClass(ctx, (int)js_image_class_id);
    if (JS_IsException(obj)) return obj;
    auto* img = new ImageData();
    img->ctx = ctx;
    JS_SetOpaque(obj, img);
    return obj;
}

bool ImageBindings::getImagePixels(JSValue val, ImagePixels& out) {
    auto* img = static_cast<ImageData*>(JS_GetOpaque(val, js_image_class_id));
    if (!img || !img->complete || img->pixels.empty()) return false;
    out.data = img->pixels.data();
    out.width = img->width;
    out.height = img->height;
    return true;
}

} // namespace bro::js

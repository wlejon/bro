#include "js/image_bindings.h"

#include <quickjs.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace bro::js {

static JSClassID js_image_class_id = 0;

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
        if (!JS_IsUndefined(img->onload)) {
            // Note: can't free JS values from finalizer safely in all cases,
            // but QuickJS handles this during GC.
        }
        delete img;
    }
}

static JSClassDef js_image_class = {
    "Image", js_image_finalizer, nullptr, nullptr, nullptr
};

static inline ImageData* getImage(JSValueConst val) {
    return static_cast<ImageData*>(JS_GetOpaque(val, js_image_class_id));
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
    if (s) {
        img->src = s;
        JS_FreeCString(ctx, s);

        // TODO: Load the image from disk (stb_image or Skia codec).
        // For now, mark as complete with 1x1 white pixel so three.js
        // doesn't hang waiting for onload.
        img->width = 1;
        img->height = 1;
        img->pixels = {255, 255, 255, 255};
        img->complete = true;

        // Fire onload callback if set
        if (JS_IsFunction(ctx, img->onload)) {
            JSValue ret = JS_Call(ctx, img->onload, this_val, 0, nullptr);
            JS_FreeValue(ctx, ret);
        }
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

static const JSCFunctionListEntry js_image_proto_funcs[] = {
    JS_CGETSET_DEF("width", js_image_get_width, nullptr),
    JS_CGETSET_DEF("height", js_image_get_height, nullptr),
    JS_CGETSET_DEF("naturalWidth", js_image_get_naturalWidth, nullptr),
    JS_CGETSET_DEF("naturalHeight", js_image_get_naturalHeight, nullptr),
    JS_CGETSET_DEF("src", js_image_get_src, js_image_set_src),
    JS_CGETSET_DEF("complete", js_image_get_complete, nullptr),
    JS_CGETSET_DEF("onload", js_image_get_onload, js_image_set_onload),
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

void ImageBindings::install(JSContext* ctx) {
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

} // namespace bro::js

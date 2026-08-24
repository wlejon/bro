#include "js/menu_bindings.h"
#include "engine/engine.h"
#include "engine/menu_bar.h"
#include <cstring>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

static const char* kMenuEngineKey = "__bro_menu_engine_ptr";

static engine::Engine* getEngine(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kMenuEngineKey);
    engine::Engine* e = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        e = reinterpret_cast<engine::Engine*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return e;
}

static std::string readCString(JSContext* ctx, JSValueConst v) {
    std::string out;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); }
    }
    return out;
}

static JSValue js_menu_show(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    eng->menuBar().visible = true;
    eng->menuBar().dirty = true;
    eng->onMenuChanged();
    return JS_UNDEFINED;
}

static JSValue js_menu_hide(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    eng->menuBar().visible = false;
    eng->menuBar().dirty = true;
    eng->onMenuChanged();
    return JS_UNDEFINED;
}

static JSValue js_menu_get_visible(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* eng = getEngine(ctx);
    return JS_NewBool(ctx, eng ? eng->menuBar().visible : false);
}

static JSValue js_menu_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsArray(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.menu.set() requires an array");
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    eng->menuBar().setRootsFromJS(ctx, argv[0]);
    eng->onMenuChanged();
    return JS_UNDEFINED;
}

static JSValue js_menu_addItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "bro.menu.addItem(parentId, item [, index])");
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;

    std::string parentId = readCString(ctx, argv[0]);
    if (!JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "item must be an object");

    std::vector<engine::MenuBar::Item> parsed;
    {
        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, argv[1]));
        engine::MenuBar tmp;
        tmp.setRootsFromJS(ctx, arr);
        parsed = std::move(tmp.roots);
        JS_FreeValue(ctx, arr);
    }
    if (parsed.empty()) return JS_FALSE;

    int index = -1;
    if (argc >= 3 && JS_IsNumber(argv[2])) {
        int32_t i = 0; JS_ToInt32(ctx, &i, argv[2]); index = i;
    }
    bool ok = eng->menuBar().addItem(parentId, std::move(parsed[0]), index);
    if (ok) eng->onMenuChanged();
    return JS_NewBool(ctx, ok);
}

static JSValue js_menu_updateItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "bro.menu.updateItem(id, props)");
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    std::string id = readCString(ctx, argv[0]);
    bool ok = eng->menuBar().updateItem(ctx, id, argv[1]);
    if (ok) eng->onMenuChanged();
    return JS_NewBool(ctx, ok);
}

static JSValue js_menu_removeItem(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    std::string id = readCString(ctx, argv[0]);
    bool ok = eng->menuBar().removeItem(id);
    if (ok) eng->onMenuChanged();
    return JS_NewBool(ctx, ok);
}

static JSValue js_menu_on(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "bro.menu.on(id, fn)");
    auto* eng = getEngine(ctx);
    if (!eng) return JS_UNDEFINED;
    std::string id = readCString(ctx, argv[0]);
    eng->menuBar().on(ctx, id, argv[1]);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_menu_funcs[] = {
    JS_CFUNC_DEF("show", 0, js_menu_show),
    JS_CFUNC_DEF("hide", 0, js_menu_hide),
    JS_CFUNC_DEF("set", 1, js_menu_set),
    JS_CFUNC_DEF("addItem", 3, js_menu_addItem),
    JS_CFUNC_DEF("updateItem", 2, js_menu_updateItem),
    JS_CFUNC_DEF("removeItem", 1, js_menu_removeItem),
    JS_CFUNC_DEF("on", 2, js_menu_on),
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void MenuBindings::install(JSContext* ctx, engine::Engine* engine) {
    JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, kMenuEngineKey,
                          JS_NewInt64(ctx, static_cast<int64_t>(
                              reinterpret_cast<intptr_t>(engine))));

        JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
        if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
            broObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
        }

        JSValue menuObj = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, menuObj, js_menu_funcs,
                                   sizeof(js_menu_funcs) / sizeof(js_menu_funcs[0]));

        JSAtom visibleAtom = JS_NewAtom(ctx, "visible");
        JS_DefinePropertyGetSet(ctx, menuObj, visibleAtom,
            JS_NewCFunction(ctx, js_menu_get_visible, "get visible", 0),
            JS_UNDEFINED,
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, visibleAtom);

        JS_SetPropertyStr(ctx, broObj, "menu", menuObj);
        JS_FreeValue(ctx, broObj);
        JS_FreeValue(ctx, global);
}


} // namespace bro::js

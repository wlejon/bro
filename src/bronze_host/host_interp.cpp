// The interpreter bridge. host_interp.h says why it exists; this file is the
// mechanism, and it is four things stacked:
//
//   1. THE CROSSING TABLE — one row per object that has been through the
//      boundary, holding both halves. It answers identity in both directions,
//      and it is what makes a round trip give back the original rather than a
//      wrapper around a wrapper.
//   2. toJs / toBronze — the conversion pair. Primitives copy. Objects wrap.
//   3. THE TWO WRAPPERS — a QuickJS class whose property access forwards into
//      bronze, and a bronze function/proxy whose property access forwards into
//      QuickJS.
//   4. THE HOOK — CreateDynamicFunction, performed by JS_Eval.
//
// WHY THE TABLE OWNS EVERYTHING. Both wrappers are reachable from a finalizer:
// a bronze function object dies and destroys its closure, a QuickJS object
// dies and runs its class finalizer. Neither may touch the other collector —
// host_internal.h's GC rule forbids an embed call from a bronze finalizer, and
// the mirror hazard is worse (a QuickJS finalizer destroying an ev::Persistent
// re-enters bronze mid-sweep). So no wrapper owns a reference to anything:
// each holds an INDEX, the table holds the references, and both finalizers do
// nothing at all.

#include "bronze_host/host_interp.h"

#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "dom/element.h"
#include "js/dom_bindings.h"
#include "js/runtime.h"
#include "util/log.h"

#include "quickjs.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// The crossing table
// ---------------------------------------------------------------------------

struct Crossing {
    ev::Persistent bronze;      // rooted; the collector updates it in place
    JSValue js = JS_UNDEFINED;  // owned (one JS_DupValue), freed on reset
};

struct Bridge {
    JSContext* ctx = nullptr;
    JSClassID refClass = 0;
    std::vector<std::unique_ptr<Crossing>> rows;
    // The JS half is a stable pointer — QuickJS never moves an object — so
    // that direction gets a real map. The bronze half cannot: embed.h says raw
    // bits "name a pre-collection address", so a map keyed on them would be
    // wrong after the first collection and wrong SILENTLY. That direction
    // scans instead, comparing each row's CURRENT bits through its Persistent,
    // which is always right. The scan is over objects that have actually
    // crossed — a handful for a scripted scene — and the day that stops being
    // true the fix is an identity primitive in embed, not a cache that can go
    // stale.
    std::unordered_map<void*, size_t> byJs;
};

Bridge& bridge() {
    static Bridge b;
    return b;
}

Crossing* rowForJs(JSValueConst v) {
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return nullptr;
    Bridge& b = bridge();
    auto it = b.byJs.find(JS_VALUE_GET_PTR(v));
    return it == b.byJs.end() ? nullptr : b.rows[it->second].get();
}

Crossing* rowForBronze(Value v) {
    Bridge& b = bridge();
    const uint64_t want = ev::toBits(v);
    for (auto& row : b.rows) {
        if (ev::toBits(row->bronze.get()) == want) return row.get();
    }
    return nullptr;
}

// Both halves in, one row out. `js` is duplicated here: the table owns its
// reference for the bridge's life, independently of whoever handed it in.
size_t addCrossing(Value bronze, JSValueConst js) {
    Bridge& b = bridge();
    auto row = std::make_unique<Crossing>();
    row->bronze.set(bronze);
    row->js = JS_DupValue(b.ctx, js);
    const size_t index = b.rows.size();
    if (JS_VALUE_GET_TAG(js) == JS_TAG_OBJECT) b.byJs[JS_VALUE_GET_PTR(js)] = index;
    b.rows.push_back(std::move(row));
    return index;
}

JSValue toJs(Value v);
Value toBronze(JSValueConst v);

// ---------------------------------------------------------------------------
// Errors, both ways
// ---------------------------------------------------------------------------

// A QuickJS exception, described well enough for a bronze throw to carry it.
std::string describeJsException(JSContext* ctx) {
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    std::string out = s ? s : "(unknown error)";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    return out;
}

// A thrown bronze value, raised into QuickJS. The message is the whole of what
// crosses: an Error's class and stack belong to the heap it was made in, and
// wrapping it would let a `catch` on the other side mutate the original.
JSValue throwIntoJs(JSContext* ctx, Value thrown) {
    std::string message;
    if (ev::isObject(thrown)) {
        Value m = ev::getProperty(thrown, "message");
        message = ev::isUndefined(m) ? std::string("(error)") : ev::toUtf8(m);
    } else {
        message = ev::isUndefined(thrown) ? std::string("(error)") : ev::toUtf8(thrown);
    }
    return JS_ThrowInternalError(ctx, "%s", message.c_str());
}

// ---------------------------------------------------------------------------
// The QuickJS side of a bronze object
// ---------------------------------------------------------------------------
//
// An exotic class rather than a Proxy: a Proxy's invariant checks read its
// TARGET, and there is no target here that could answer for a compiled object.
// The exotic hooks are consulted directly and answer from bronze every time,
// which is what keeps the view LIVE — a script that reads `scene.children`
// after adding to it sees the addition.

// Stored +1, so a fresh object (opaque nullptr) is distinguishable from row 0.
size_t indexOfJsRef(JSValueConst obj) {
    void* p = JS_GetOpaque(obj, bridge().refClass);
    return reinterpret_cast<size_t>(p) - 1;
}

bool isJsRef(JSValueConst obj) {
    return JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT &&
           JS_GetClassID(obj) == bridge().refClass;
}

Value bronzeBehind(JSValueConst obj) {
    return bridge().rows[indexOfJsRef(obj)]->bronze.get();
}

JSValue jsRefGet(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst) {
    const char* key = JS_AtomToCString(ctx, atom);
    if (!key) return JS_EXCEPTION;
    Value out = ev::getProperty(bronzeBehind(obj), key);
    JS_FreeCString(ctx, key);
    return toJs(out);
}

int jsRefSet(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
             JSValueConst, int) {
    const char* key = JS_AtomToCString(ctx, atom);
    if (!key) return -1;
    ev::setProperty(bronzeBehind(obj), key, toBronze(value));
    JS_FreeCString(ctx, key);
    return true;
}

int jsRefHas(JSContext* ctx, JSValueConst obj, JSAtom atom) {
    const char* key = JS_AtomToCString(ctx, atom);
    if (!key) return -1;
    // `in` is prototype-inclusive, and so is getProperty. A property whose
    // value genuinely IS undefined reads as absent; that is the approximation
    // `key in obj` already makes for a host object with no [[HasProperty]] of
    // its own, and the alternative — Reflect.has — costs a call into compiled
    // code on every membership test a script performs.
    const bool present = !ev::isUndefined(ev::getProperty(bronzeBehind(obj), key));
    JS_FreeCString(ctx, key);
    return present ? 1 : 0;
}

// `Object.keys` does NOT stop at the name list below. It walks
// get_own_property_names and then asks get_own_property for each name, because
// enumerability is a property of the DESCRIPTOR — quickjs.h says as much where
// it notes that the name list's `is_enumerable` field is ignored. Without this
// hook every name was filtered back out and `Object.keys` of a compiled object
// answered the empty list while `o.a` read fine, which is the confusing shape
// of half a protocol.
//
// Every name the list produced reports the same descriptor: an enumerable,
// writable, configurable data property. `hasOwnProperty` therefore answers for
// anything reachable on the prototype chain too — getProperty is
// chain-inclusive and there is no own-only read in the embed surface — which
// is the same approximation has_property already makes, in the same direction.
int jsRefGetOwnProperty(JSContext* ctx, JSPropertyDescriptor* desc, JSValueConst obj,
                        JSAtom prop) {
    const char* key = JS_AtomToCString(ctx, prop);
    if (!key) return -1;
    Value v = ev::getProperty(bronzeBehind(obj), key);
    JS_FreeCString(ctx, key);
    if (ev::isUndefined(v)) return 0;
    if (desc) {
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        desc->value = toJs(v);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

// The name list itself. Reflect.ownKeys is the answer bronze would give.
int jsRefOwnKeys(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen,
                 JSValueConst obj) {
    *ptab = nullptr;
    *plen = 0;
    ev::GlobalValue reflect = ev::globalValue("Reflect");
    if (!reflect.found) return 0;
    Value fn = ev::getProperty(reflect.value, "ownKeys");
    if (!ev::isFunction(fn)) return 0;
    Value self = bronzeBehind(obj);
    ev::CallResult r = ev::call(fn, reflect.value, std::span<const Value>(&self, 1));
    if (r.thrown || !ev::isObject(r.value)) return 0;

    const uint32_t n =
        static_cast<uint32_t>(ev::toDouble(ev::getProperty(r.value, "length")));
    if (n == 0) return 0;
    auto* tab = static_cast<JSPropertyEnum*>(js_malloc(ctx, sizeof(JSPropertyEnum) * n));
    if (!tab) return -1;
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; ++i) {
        Value k = ev::getElement(r.value, i);
        // A symbol key has no string spelling and toUtf8 of one is a spec
        // TypeError (host_proxy.cpp learned this the hard way); skip it rather
        // than turn an enumeration into a throw.
        if (ev::isSymbol(k)) continue;
        tab[written].atom = JS_NewAtom(ctx, ev::toUtf8(k).c_str());
        tab[written].is_enumerable = 1;
        ++written;
    }
    *ptab = tab;
    *plen = written;
    return 0;
}

// A compiled object may be a FUNCTION, and a script handed one calls it.
// `flags` carries JS_CALL_FLAG_CONSTRUCTOR for `new`.
JSValue jsRefCall(JSContext* ctx, JSValueConst func_obj, JSValueConst this_val,
                  int argc, JSValueConst* argv, int flags) {
    std::vector<Value> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(toBronze(argv[i]));

    Value fn = bronzeBehind(func_obj);
    ev::CallResult r = (flags & JS_CALL_FLAG_CONSTRUCTOR)
                           ? ev::construct(fn, std::span<const Value>(args))
                           : ev::call(fn, toBronze(this_val), std::span<const Value>(args));
    if (r.thrown) return throwIntoJs(ctx, r.value);
    return toJs(r.value);
}

JSClassExoticMethods g_refExotic = {
    /* get_own_property       */ jsRefGetOwnProperty,
    /* get_own_property_names */ jsRefOwnKeys,
    /* delete_property        */ nullptr,
    /* define_own_property    */ nullptr,
    /* has_property           */ jsRefHas,
    /* get_property           */ jsRefGet,
    /* set_property           */ jsRefSet,
};

JSClassDef g_refClassDef = {
    /* class_name */ "BronzeRef",
    // Nothing to finalize: the crossing table owns both halves, so a wrapper
    // that dies takes nothing with it. See the file header.
    /* finalizer */ nullptr,
    /* gc_mark   */ nullptr,
    /* call      */ jsRefCall,
    /* exotic    */ &g_refExotic,
};

JSValue makeJsRef(Value bronze) {
    Bridge& b = bridge();
    JSValue obj = JS_NewObjectClass(b.ctx, static_cast<int>(b.refClass));
    if (JS_IsException(obj)) return obj;
    const size_t index = addCrossing(bronze, obj);
    JS_SetOpaque(obj, reinterpret_cast<void*>(index + 1));
    return obj;
}

// ---------------------------------------------------------------------------
// The bronze side of a QuickJS object
// ---------------------------------------------------------------------------

// Calling an interpreted function, in one place: the two trap shapes and the
// direct wrapper all funnel through it.
Value callInterpreted(size_t rowIndex, Value thisValue, std::span<const Value> args,
                      bool asConstructor) {
    Bridge& b = bridge();
    std::vector<JSValue> jsArgs;
    jsArgs.reserve(args.size());
    for (Value a : args) jsArgs.push_back(toJs(a));
    JSValue jsThis = toJs(thisValue);
    JSValue fn = b.rows[rowIndex]->js;

    JSValue r = asConstructor
                    ? JS_CallConstructor(b.ctx, fn, static_cast<int>(jsArgs.size()),
                                         jsArgs.empty() ? nullptr : jsArgs.data())
                    : JS_Call(b.ctx, fn, jsThis, static_cast<int>(jsArgs.size()),
                              jsArgs.empty() ? nullptr : jsArgs.data());
    for (JSValue a : jsArgs) JS_FreeValue(b.ctx, a);
    JS_FreeValue(b.ctx, jsThis);

    if (JS_IsException(r)) {
        JS_FreeValue(b.ctx, r);
        return ev::throwError("interpreted function: " + describeJsException(b.ctx));
    }
    Value out = toBronze(r);
    JS_FreeValue(b.ctx, r);
    return out;
}

// A function is what a script hands BACK — the editor's APP scripts return
// `{ init, update }` and the compiled player calls those every frame — and it
// is also what a vendor library IS. Both want the same thing and the second
// wants more of it: `CodeMirror` is called AND carries `CodeMirror.Pass`,
// `CodeMirror.commands`, `CodeMirror.fromTextArea`. A bare host function
// forwards the call and nothing else, so this is a Proxy whose TARGET is a
// callable — the only way [[Call]] and property forwarding can both be true of
// one value.
//
// The target is deliberately UNNAMED: a named host function has own `name` and
// `length` properties, and the 10.5 get-invariant would then check the trap's
// answer against them. Unnamed, there is nothing to disagree with, and `name`
// is answered from the interpreted function like every other property.
Value makeBronzeFunction(size_t rowIndex) {
    HostProxyTraps t;
    t.target = ev::makeFunction([](Value, std::span<const Value>) { return ev::undefined(); });
    t.get = [rowIndex](const std::string& key, Value& out) {
        Bridge& b = bridge();
        JSValue v = JS_GetPropertyStr(b.ctx, b.rows[rowIndex]->js, key.c_str());
        if (JS_IsException(v)) {
            JS_FreeValue(b.ctx, v);
            (void)describeJsException(b.ctx);
            return false;
        }
        if (JS_IsUndefined(v)) {
            JS_FreeValue(b.ctx, v);
            return false;
        }
        out = toBronze(v);
        JS_FreeValue(b.ctx, v);
        return true;
    };
    t.set = [rowIndex](const std::string& key, Value v) {
        Bridge& b = bridge();
        JS_SetPropertyStr(b.ctx, b.rows[rowIndex]->js, key.c_str(), toJs(v));
    };
    t.has = [rowIndex](const std::string& key) {
        Bridge& b = bridge();
        JSAtom atom = JS_NewAtom(b.ctx, key.c_str());
        const int r = JS_HasProperty(b.ctx, b.rows[rowIndex]->js, atom);
        JS_FreeAtom(b.ctx, atom);
        return r > 0;
    };
    t.apply = [rowIndex](Value thisValue, std::span<const Value> args) {
        return callInterpreted(rowIndex, thisValue, args, false);
    };
    t.construct = [rowIndex](std::span<const Value> args) {
        return callInterpreted(rowIndex, ev::undefined(), args, true);
    };
    return makeHostProxy(std::move(t));
}

// Everything else: a proxy whose traps forward. host_proxy.cpp explains why the
// target must stay empty and why symbol keys need guarding; this reuses both.
Value makeBronzeObject(size_t rowIndex) {
    HostProxyTraps t;
    t.get = [rowIndex](const std::string& key, Value& out) {
        Bridge& b = bridge();
        JSValue v = JS_GetPropertyStr(b.ctx, b.rows[rowIndex]->js, key.c_str());
        if (JS_IsException(v)) {
            JS_FreeValue(b.ctx, v);
            (void)describeJsException(b.ctx);
            return false;
        }
        if (JS_IsUndefined(v)) {
            JS_FreeValue(b.ctx, v);
            return false;
        }
        out = toBronze(v);
        JS_FreeValue(b.ctx, v);
        return true;
    };
    t.set = [rowIndex](const std::string& key, Value v) {
        Bridge& b = bridge();
        JS_SetPropertyStr(b.ctx, b.rows[rowIndex]->js, key.c_str(), toJs(v));
    };
    t.has = [rowIndex](const std::string& key) {
        Bridge& b = bridge();
        JSAtom atom = JS_NewAtom(b.ctx, key.c_str());
        const int r = JS_HasProperty(b.ctx, b.rows[rowIndex]->js, atom);
        JS_FreeAtom(b.ctx, atom);
        return r > 0;
    };
    t.ownKeys = [rowIndex]() {
        Bridge& b = bridge();
        std::vector<std::string> keys;
        JSPropertyEnum* tab = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(b.ctx, &tab, &len, b.rows[rowIndex]->js,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
            return keys;
        }
        for (uint32_t i = 0; i < len; ++i) {
            const char* s = JS_AtomToCString(b.ctx, tab[i].atom);
            if (s) {
                keys.emplace_back(s);
                JS_FreeCString(b.ctx, s);
            }
            JS_FreeAtom(b.ctx, tab[i].atom);
        }
        js_free(b.ctx, tab);
        return keys;
    };
    t.remove = [rowIndex](const std::string& key) {
        Bridge& b = bridge();
        JSAtom atom = JS_NewAtom(b.ctx, key.c_str());
        JS_DeleteProperty(b.ctx, b.rows[rowIndex]->js, atom, 0);
        JS_FreeAtom(b.ctx, atom);
    };
    return makeHostProxy(std::move(t));
}

// ---------------------------------------------------------------------------
// The conversion pair
// ---------------------------------------------------------------------------

// Binary data is COPIED, and that is forced rather than chosen. embed.h's
// pointer contract says a typed array's bytes live in the moving bronze heap
// and the address dies at the next allocation — so a QuickJS view sharing that
// buffer would be a dangling read the first time either side allocated, which
// is every frame. The copy means a script that MUTATES an array it was handed
// mutates its own copy; three.js scripts read geometry far more than they
// write it, and a shared-buffer story needs a pinning primitive bronze does
// not have.

struct KindMap {
    bronze::ElementKind kind;
    const char* jsCtor;
};

const KindMap kKinds[] = {
    {ev::elements::Int8, "Int8Array"},         {ev::elements::Uint8, "Uint8Array"},
    {ev::elements::Uint8Clamped, "Uint8ClampedArray"},
    {ev::elements::Int16, "Int16Array"},       {ev::elements::Uint16, "Uint16Array"},
    {ev::elements::Int32, "Int32Array"},       {ev::elements::Uint32, "Uint32Array"},
    {ev::elements::Float32, "Float32Array"},   {ev::elements::Float64, "Float64Array"},
};

JSValue typedArrayToJs(Value v) {
    Bridge& b = bridge();
    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
    if (!info) return JS_UNDEFINED;
    const char* ctorName = nullptr;
    for (const KindMap& k : kKinds) {
        if (k.kind == info.elementKind) ctorName = k.jsCtor;
    }
    // Float16 and the two BigInt kinds have no plain-number counterpart worth
    // faking; they arrive as a byte view rather than as a wrong element type.
    std::vector<uint8_t> bytes(info.data, info.data + info.byteLength);

    JSValue global = JS_GetGlobalObject(b.ctx);
    JSValue ctor = JS_GetPropertyStr(b.ctx, global, ctorName ? ctorName : "Uint8Array");
    JS_FreeValue(b.ctx, global);
    JSValue count = JS_NewInt64(
        b.ctx, ctorName ? info.elementCount : static_cast<int64_t>(bytes.size()));
    JSValue out = JS_CallConstructor(b.ctx, ctor, 1, &count);
    JS_FreeValue(b.ctx, count);
    JS_FreeValue(b.ctx, ctor);
    if (JS_IsException(out)) return out;

    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buffer =
        JS_GetTypedArrayBuffer(b.ctx, out, &byteOffset, &byteLength, &bytesPerElement);
    size_t bufLen = 0;
    uint8_t* dst = JS_GetArrayBuffer(b.ctx, &bufLen, buffer);
    if (dst && byteOffset + bytes.size() <= bufLen)
        std::memcpy(dst + byteOffset, bytes.data(), bytes.size());
    JS_FreeValue(b.ctx, buffer);
    return out;
}

Value typedArrayToBronze(JSValueConst v) {
    Bridge& b = bridge();
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buffer =
        JS_GetTypedArrayBuffer(b.ctx, v, &byteOffset, &byteLength, &bytesPerElement);
    if (JS_IsException(buffer)) {
        JS_FreeValue(b.ctx, buffer);
        JS_GetException(b.ctx);
        return ev::undefined();
    }
    size_t bufLen = 0;
    uint8_t* src = JS_GetArrayBuffer(b.ctx, &bufLen, buffer);
    std::vector<uint8_t> bytes;
    if (src && byteOffset + byteLength <= bufLen)
        bytes.assign(src + byteOffset, src + byteOffset + byteLength);
    JS_FreeValue(b.ctx, buffer);

    // The element kind is read off the constructor's name, which is the only
    // thing QuickJS reports about a view without a per-kind accessor per type.
    JSValue ctor = JS_GetPropertyStr(b.ctx, v, "constructor");
    JSValue nameV = JS_GetPropertyStr(b.ctx, ctor, "name");
    const char* nameC = JS_ToCString(b.ctx, nameV);
    const std::string name = nameC ? nameC : "";
    if (nameC) JS_FreeCString(b.ctx, nameC);
    JS_FreeValue(b.ctx, nameV);
    JS_FreeValue(b.ctx, ctor);

    bronze::ElementKind kind = ev::elements::Uint8;
    uint32_t perElement = 1;
    for (const KindMap& k : kKinds) {
        if (name == k.jsCtor) {
            kind = k.kind;
            perElement = static_cast<uint32_t>(bytesPerElement ? bytesPerElement : 1);
        }
    }
    Value view = ev::createTypedArray(
        kind, static_cast<uint32_t>(bytes.size() / (perElement ? perElement : 1)));
    ev::fillTypedArray(view, std::span<const uint8_t>(bytes));
    return view;
}


JSValue toJs(Value v) {
    Bridge& b = bridge();
    if (ev::isUndefined(v)) return JS_UNDEFINED;
    if (ev::isNull(v)) return JS_NULL;
    // No spelling survives the trip, and toUtf8 of a symbol is a spec
    // TypeError. Undefined is the honest answer; a wrapper would be a symbol
    // that is not `===` to the one it stands for, which is the only property a
    // symbol has.
    if (ev::isSymbol(v)) return JS_UNDEFINED;
    if (ev::isBool(v)) return JS_NewBool(b.ctx, ev::toBool(v) ? 1 : 0);
    if (ev::isNumber(v)) return JS_NewFloat64(b.ctx, ev::toDouble(v));
    if (ev::isString(v)) return JS_NewString(b.ctx, ev::toUtf8(v).c_str());
    if (!ev::isObject(v)) return JS_UNDEFINED;  // BigInt and the holes
    if (ev::isTypedArray(v)) return typedArrayToJs(v);
    // An ENGINE object is not wrapped in either direction: both realms already
    // wrap the same dom::Element, so the crossing resolves to the other
    // realm's existing wrapper for it. This is the README's rule ("Engine
    // objects are shared") reaching the bridge — and it is not an optimisation
    // but the difference between working and not: appendChild on either side
    // unwraps to a dom::Node, and a generic forwarding wrapper is not one.
    if (dom::Element* el = hostElementOf(v))
        return js::DomBindings::wrapElement(b.ctx, el);
    if (Crossing* row = rowForBronze(v)) return JS_DupValue(b.ctx, row->js);
    return makeJsRef(v);
}

Value toBronze(JSValueConst v) {
    Bridge& b = bridge();
    switch (JS_VALUE_GET_NORM_TAG(v)) {
        case JS_TAG_UNDEFINED: return ev::undefined();
        case JS_TAG_NULL: return ev::null();
        case JS_TAG_BOOL: return ev::fromBool(JS_ToBool(b.ctx, v) != 0);
        case JS_TAG_INT: return ev::fromDouble(JS_VALUE_GET_INT(v));
        case JS_TAG_FLOAT64: return ev::fromDouble(JS_VALUE_GET_FLOAT64(v));
        case JS_TAG_STRING: {
            const char* s = JS_ToCString(b.ctx, v);
            Value out = ev::fromUtf8(s ? s : "");
            if (s) JS_FreeCString(b.ctx, s);
            return out;
        }
        default: break;
    }
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return ev::undefined();

    // A bronze object that went out and came back is itself again, not a
    // wrapper around its wrapper. This is the whole reason the table is keyed
    // in both directions.
    if (isJsRef(v)) return bronzeBehind(v);
    // The other half of the engine-object rule above. A <div> CodeMirror built
    // in the interpreted realm and handed to a compiled appendChild must
    // arrive as that element, not as a proxy that merely forwards `nodeType`.
    if (auto* el = static_cast<dom::Element*>(js::DomBindings::unwrapElement(b.ctx, v)))
        return hostElementValue(el);
    if (Crossing* row = rowForJs(v)) return row->bronze.get();

    // Binary data copies rather than wraps, for the reason typedArrayToJs
    // gives; it is also the one object kind where a wrapper would be wrong on
    // its own terms, since element access on a typed array is not a property
    // read the traps could serve.
    {
        size_t off = 0, len = 0, per = 0;
        JSValue buf = JS_GetTypedArrayBuffer(b.ctx, v, &off, &len, &per);
        const bool isView = !JS_IsException(buf);
        if (isView) {
            JS_FreeValue(b.ctx, buf);
            return typedArrayToBronze(v);
        }
        JS_FreeValue(b.ctx, buf);
        JS_GetException(b.ctx);
    }

    const size_t index = b.rows.size();
    Value wrapper =
        JS_IsFunction(b.ctx, v) ? makeBronzeFunction(index) : makeBronzeObject(index);
    addCrossing(wrapper, v);
    return wrapper;
}

// ---------------------------------------------------------------------------
// CreateDynamicFunction (27.3.1.1), performed by the interpreter
// ---------------------------------------------------------------------------

const char* sourcePrefixFor(ev::DynamicFunctionKind kind) {
    switch (kind) {
        case ev::DynamicFunctionKind::Generator: return "(function* anonymous(";
        case ev::DynamicFunctionKind::Async: return "(async function anonymous(";
        case ev::DynamicFunctionKind::AsyncGenerator: return "(async function* anonymous(";
        default: return "(function anonymous(";
    }
}

Value dynamicFunction(ev::DynamicFunctionKind kind, std::span<const Value> args) {
    Bridge& b = bridge();
    if (!b.ctx) {
        return ev::throwError(
            "new Function: no interpreter realm to compile in (the bridge is not installed)");
    }

    // 27.3.1.1 steps 8-12: every argument but the last is a parameter list,
    // joined with commas; the last is the body. No arguments at all is an
    // empty function, which is what the spec's empty defaults produce.
    std::string params;
    std::string body;
    for (size_t i = 0; i < args.size(); ++i) {
        const bool last = (i + 1 == args.size());
        std::string text =
            ev::isUndefined(args[i]) ? std::string("undefined") : ev::toUtf8(args[i]);
        if (last) {
            body = std::move(text);
        } else {
            if (!params.empty()) params += ",";
            params += text;
        }
    }

    // The parenthesised form, so JS_Eval sees an expression and hands back the
    // function rather than declaring it. `anonymous` is the name 27.3.1.1
    // gives these, and library code reads it.
    const std::string source = std::string(sourcePrefixFor(kind)) + params + "\n) {\n" +
                               body + "\n})";

    JSValue fn =
        JS_Eval(b.ctx, source.c_str(), source.size(), "<new Function>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JS_FreeValue(b.ctx, fn);
        return ev::throwError("new Function: " + describeJsException(b.ctx));
    }
    Value out = toBronze(fn);
    JS_FreeValue(b.ctx, fn);
    return out;
}

}  // namespace

void installInterpBridge(engine::Engine& engine) {
    Bridge& b = bridge();
    js::Runtime* rt = engine.jsRuntime();
    if (!rt || !rt->getContext()) {
        LOG_WARN("bronze_host: no QuickJS realm; compiled `new Function` will refuse");
        return;
    }
    b.ctx = rt->getContext();
    if (b.refClass == 0) {
        JS_NewClassID(JS_GetRuntime(b.ctx), &b.refClass);
        JS_NewClass(JS_GetRuntime(b.ctx), b.refClass, &g_refClassDef);
    }
    ev::setDynamicFunctionHook(dynamicFunction);
    LOG_INFO("bronze_host: interpreter bridge installed (compiled `new Function` "
             "compiles in the app's QuickJS realm)");
}

Value bridgeJsGlobal(const char* name) {
    Bridge& b = bridge();
    if (!b.ctx) return ev::undefined();
    JSValue global = JS_GetGlobalObject(b.ctx);
    JSValue v = JS_GetPropertyStr(b.ctx, global, name);
    JS_FreeValue(b.ctx, global);
    if (JS_IsException(v)) {
        JS_FreeValue(b.ctx, v);
        (void)describeJsException(b.ctx);
        return ev::undefined();
    }
    Value out = toBronze(v);
    JS_FreeValue(b.ctx, v);
    return out;
}

void resetInterpBridge() {
    Bridge& b = bridge();
    for (auto& row : b.rows) {
        if (b.ctx) JS_FreeValue(b.ctx, row->js);
    }
    b.rows.clear();
    b.byJs.clear();
}

}  // namespace bro::bronze_host

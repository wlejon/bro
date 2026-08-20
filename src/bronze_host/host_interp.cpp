// The interpreter bridge. host_interp.h says why it exists; this file is the
// mechanism, and it is five things stacked:
//
//   1. THE CROSSING TABLE — one row per object that has been through the
//      boundary, holding both halves. It answers identity in both directions,
//      and it is what makes a round trip give back the original rather than a
//      wrapper around a wrapper.
//   2. toJs / toBronze — the conversion pair. Primitives copy. Objects wrap.
//      Typed arrays SHARE: both realms hold windows over one external byte
//      store (embed.h's externalizeArrayBuffer), so a script's write to
//      `position.array[0]` is the compiled program's write too.
//   3. THE TWO WRAPPERS — a QuickJS class whose property access forwards into
//      bronze, and a bronze function/proxy whose property access forwards into
//      QuickJS.
//   4. THE HOOKS — CreateDynamicFunction and eval, performed by JS_Eval.
//   5. THE SWEEP — sweepInterpBridge, the frame-boundary reclaim that keeps
//      the table from being the leak it used to be.
//
// OWNERSHIP, per row: each row OWNS the foreign half and holds its own-side
// wrapper without owning it, so the wrapper can die when the program lets go
// — and the wrapper's death is the row's reclaim signal:
//
//   - A bronze object crossed OUT (BronzeOut) is rooted by a Persistent; the
//     QuickJS wrapper is held un-duplicated, and its class FINALIZER pushes
//     the row index onto a plain vector. The finalizer touches nothing else —
//     host_internal.h's GC rule forbids an embed call from a finalizer, and
//     pushing an integer is not one.
//   - A QuickJS object crossed IN (JsIn) is owned by one JS_DupValue; the
//     bronze proxy is held through a bronze WeakRef (itself in a Persistent),
//     so the row learns the proxy died by deref'ing at the sweep. The same
//     shape carries the shared typed-array rows (SharedBuf, SharedView).
//
// The sweep runs from hostFrame — a plain host stack, outside both
// collectors — which is where JS_FreeValue and Persistent release are
// unconditionally safe. resetInterpBridge() remains the app-realm teardown.
//
// IDENTITY without the old linear scan: lookups by bronze value key a map on
// raw bits, rebuilt when embed's relocationEpoch() has moved — bits only
// change when that does, so the cache cannot go stale (embed.h says this is
// the primitive's whole purpose). The JS direction keys on QuickJS object
// pointers, which never move; entries are owned or flagged dead before their
// pointer can be reused.

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

enum class CrossKind : uint8_t {
    Free,       // on the free list; every field reset
    BronzeOut,  // bronze object, wrapped as a BronzeRef for JS
    JsIn,       // QuickJS object, wrapped as a HostProxy for bronze
    SharedBuf,  // one external byte store, an ArrayBuffer in both realms
    SharedView, // one typed-array window, a view object in both realms
};

struct Crossing {
    CrossKind kind = CrossKind::Free;
    // BronzeOut: set by the wrapper's finalizer; the row is garbage awaiting
    // the sweep, and every lookup treats it as absent — the wrapper's memory
    // may already be reused.
    bool deadPending = false;
    // BronzeOut: the foreign bronze object, rooted. The others: a bronze
    // WeakRef over the bronze-side wrapper (proxy, buffer, view), so the row
    // holds no claim on it.
    ev::Persistent bronze;
    // BronzeOut: the wrapper, NOT owned (identity only; valid while
    // !deadPending). The others: the foreign/shared JS value, owned by one
    // JS_DupValue the sweep releases.
    JSValue js = JS_UNDEFINED;
};

struct Bridge {
    JSContext* ctx = nullptr;
    std::thread::id ownerThread{};
    JSClassID refClass = 0;
    JSClassID globalsClass = 0;
    std::vector<std::unique_ptr<Crossing>> rows;
    std::vector<size_t> freeRows;
    // Indices whose BronzeOut wrapper finalized. POD only: the pushes happen
    // inside QuickJS collection.
    std::vector<size_t> pendingDead;

    // JS object pointer -> row, for the owned-js row kinds. QuickJS never
    // moves an object, and an owned entry's pointer cannot be reused while
    // the row holds its dup.
    std::unordered_map<void*, size_t> byJs;

    // Bronze bits -> row, rebuilt whenever the collector has relocated
    // anything since the last build. BronzeOut rows contribute their rooted
    // object's CURRENT bits; the weak kinds contribute their deref — a dead
    // weak simply contributes nothing until the sweep frees its row.
    std::unordered_map<uint64_t, size_t> byBronze;
    uint64_t byBronzeEpoch = ~0ULL;
};

Bridge& bridge() {
    static Bridge b;
    return b;
}

// ---- bronze WeakRef, spelled through embed ---------------------------------

Value makeWeakRef(Value target) {
    ev::GlobalValue ctor = ev::globalValue("WeakRef");
    if (!ctor.found) return ev::undefined();
    ev::CallResult r = ev::construct(ctor.value, std::span<const Value>(&target, 1));
    return r.thrown ? ev::undefined() : r.value;
}

// The referent, or undefined once the collector has proved it dead.
// getProperty MAY ALLOCATE (embed.h), so the receiver rides in a Persistent
// across it — raw bits reused after would name a pre-collection address.
Value derefWeakRef(Value weakRef) {
    if (!ev::isObject(weakRef)) return ev::undefined();
    ev::Persistent self(weakRef);
    Value deref = ev::getProperty(self.get(), "deref");
    if (!ev::isFunction(deref)) return ev::undefined();
    ev::CallResult r = ev::call(deref, self.get(), {});
    return r.thrown ? ev::undefined() : r.value;
}

// The row's bronze-side wrapper (or foreign object), as of NOW. Undefined for
// a weak row whose wrapper died.
Value bronzeHalfOf(const Crossing& row) {
    if (row.kind == CrossKind::BronzeOut) return row.bronze.get();
    return derefWeakRef(row.bronze.get());
}

void rebuildByBronze() {
    Bridge& b = bridge();
    while (b.byBronzeEpoch != ev::relocationEpoch()) {
        const uint64_t currentEpoch = ev::relocationEpoch();
        b.byBronze.clear();
        for (size_t i = 0; i < b.rows.size(); ++i) {
            const Crossing& row = *b.rows[i];
            if (row.kind == CrossKind::Free || row.deadPending) continue;
            Value v = bronzeHalfOf(row);
            if (ev::isObject(v)) b.byBronze[ev::toBits(v)] = i;
        }
        b.byBronzeEpoch = currentEpoch;
    }
}

Crossing* rowForBronze(Value v) {
    Bridge& b = bridge();
    if (b.byBronzeEpoch != ev::relocationEpoch()) rebuildByBronze();
    auto it = b.byBronze.find(ev::toBits(v));
    if (it == b.byBronze.end()) return nullptr;
    Crossing* row = b.rows[it->second].get();
    return row->deadPending || row->kind == CrossKind::Free ? nullptr : row;
}

Crossing* rowForJs(JSValueConst v) {
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return nullptr;
    Bridge& b = bridge();
    auto it = b.byJs.find(JS_VALUE_GET_PTR(v));
    if (it == b.byJs.end()) return nullptr;
    Crossing* row = b.rows[it->second].get();
    return row->deadPending || row->kind == CrossKind::Free ? nullptr : row;
}

size_t takeRow() {
    Bridge& b = bridge();
    if (!b.freeRows.empty()) {
        const size_t i = b.freeRows.back();
        b.freeRows.pop_back();
        return i;
    }
    b.rows.push_back(std::make_unique<Crossing>());
    return b.rows.size() - 1;
}

// A bronze object going OUT: the row roots it; `wrapper` is recorded without
// a dup — its finalizer is what ends the row.
size_t addBronzeOut(Value bronzeObj, JSValueConst wrapper) {
    Bridge& b = bridge();
    if (b.byBronzeEpoch != ev::relocationEpoch()) rebuildByBronze();
    const size_t index = takeRow();
    Crossing& row = *b.rows[index];
    row.kind = CrossKind::BronzeOut;
    row.deadPending = false;
    row.bronze.set(bronzeObj);
    row.js = wrapper;  // not owned
    b.byBronze[ev::toBits(bronzeObj)] = index;
    return index;
}

// A JS value coming IN, or a shared buffer/view: the row owns `js` (one dup),
// and holds `bronzeWrapper` only weakly.
size_t addWeakRow(CrossKind kind, Value bronzeWrapper, JSValueConst js) {
    Bridge& b = bridge();
    ev::Persistent wrapperP(bronzeWrapper);
    Value weakRef = makeWeakRef(wrapperP.get());
    if (b.byBronzeEpoch != ev::relocationEpoch()) rebuildByBronze();
    const size_t index = takeRow();
    Crossing& row = *b.rows[index];
    row.kind = kind;
    row.deadPending = false;
    row.bronze.set(weakRef);
    row.js = JS_DupValue(b.ctx, js);
    b.byJs[JS_VALUE_GET_PTR(js)] = index;
    b.byBronze[ev::toBits(wrapperP.get())] = index;
    return index;
}

void freeRow(size_t index, bool freeOwnedJs) {
    Bridge& b = bridge();
    Crossing& row = *b.rows[index];
    if (row.kind == CrossKind::Free) return;
    const bool ownsJs = row.kind != CrossKind::BronzeOut;
    if (ownsJs) {
        auto it = b.byJs.find(JS_VALUE_GET_PTR(row.js));
        if (it != b.byJs.end() && it->second == index) b.byJs.erase(it);
        if (freeOwnedJs && b.ctx) JS_FreeValue(b.ctx, row.js);
    }
    if (b.byBronzeEpoch == ev::relocationEpoch()) {
        Value v = bronzeHalfOf(row);
        if (ev::isObject(v)) {
            auto it = b.byBronze.find(ev::toBits(v));
            if (it != b.byBronze.end() && it->second == index) {
                b.byBronze.erase(it);
            }
        }
    }
    row.kind = CrossKind::Free;
    row.deadPending = false;
    row.bronze.set(ev::undefined());
    row.js = JS_UNDEFINED;
    b.freeRows.push_back(index);
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
    // getProperty/getElement MAY ALLOCATE (embed.h): everything read more than
    // once rides in a Persistent, or the second read is of pre-collection bits.
    ev::Persistent reflectP(reflect.value);
    Value fn = ev::getProperty(reflectP.get(), "ownKeys");
    if (!ev::isFunction(fn)) return 0;
    Value self = bronzeBehind(obj);
    ev::CallResult r = ev::call(fn, reflectP.get(), std::span<const Value>(&self, 1));
    if (r.thrown || !ev::isObject(r.value)) return 0;
    ev::Persistent arr(r.value);

    const uint32_t n =
        static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
    if (n == 0) return 0;
    auto* tab = static_cast<JSPropertyEnum*>(js_malloc(ctx, sizeof(JSPropertyEnum) * n));
    if (!tab) return -1;
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; ++i) {
        Value k = ev::getElement(arr.get(), i);
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
    // Each toBronze may allocate on the moving heap, so every earlier result
    // must already be rooted when the next runs — a plain vector<Value> here
    // was stale bits by the time the call rooted its span, and the editor's
    // Play burst (dozens of fresh crossings per frame) faulted on it. The
    // Value vector is refilled from the Persistents at the end, with nothing
    // allocating in between.
    std::vector<ev::Persistent> rooted;
    rooted.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) rooted.emplace_back(toBronze(argv[i]));
    const bool asConstructor = (flags & JS_CALL_FLAG_CONSTRUCTOR) != 0;
    ev::Persistent thisB(asConstructor ? ev::undefined() : toBronze(this_val));

    std::vector<Value> args;
    args.reserve(rooted.size());
    for (const ev::Persistent& p : rooted) args.push_back(p.get());
    Value fn = bronzeBehind(func_obj);
    ev::CallResult r = asConstructor
                           ? ev::construct(fn, std::span<const Value>(args))
                           : ev::call(fn, thisB.get(), std::span<const Value>(args));
    if (r.thrown) return throwIntoJs(ctx, r.value);
    return toJs(r.value);
}

// The reclaim signal, and the WHOLE of what a finalizer may do here: record
// the index and mark the row unreadable. It runs inside QuickJS collection —
// possibly triggered by a JS_FreeValue this very file makes — so it must not
// touch either heap; a vector push and a bool store touch neither.
void jsRefFinalizer(JSRuntime*, JSValueConst val) {
    Bridge& b = bridge();
    void* p = JS_GetOpaque(val, b.refClass);
    if (!p) return;
    const size_t index = reinterpret_cast<size_t>(p) - 1;
    if (index >= b.rows.size()) return;
    Crossing& row = *b.rows[index];
    if (row.kind != CrossKind::BronzeOut) return;
    row.deadPending = true;
    b.pendingDead.push_back(index);
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
    /* finalizer */ jsRefFinalizer,
    /* gc_mark   */ nullptr,
    /* call      */ jsRefCall,
    /* exotic    */ &g_refExotic,
};

JSValue makeJsRef(Value bronze) {
    Bridge& b = bridge();
    JSValue obj = JS_NewObjectClass(b.ctx, static_cast<int>(b.refClass));
    if (JS_IsException(obj)) return obj;
    // Without the bit, `new wrapper()` is "not a constructor" before the call
    // hook is ever consulted — a script's `new THREE.Vector3()` never reached
    // jsRefCall. Every function wrapper gets it; a bronze value that is not
    // constructible answers with its own TypeError from ev::construct, which
    // is the error the script deserves rather than the wrapper's.
    if (ev::isFunction(bronze)) JS_SetConstructorBit(b.ctx, obj, true);
    const size_t index = addBronzeOut(bronze, obj);
    JS_SetOpaque(obj, reinterpret_cast<void*>(index + 1));
    return obj;
}

// ---------------------------------------------------------------------------
// The compiled realm's globals, as the interpreted realm's outer scope
// ---------------------------------------------------------------------------
//
// Dynamic code compiles in the QuickJS realm, but the program that wrote it
// was authored against the COMPILED realm's global namespace — the editor
// assigns `window.THREE` and its scene scripts open with `new THREE.Vector3`.
// A browser has one global object; this bridge has two, so the interpreted
// one's prototype chain gains a fallback that answers from bronze:
//
//     qjs global (own properties win) -> fallback -> Object.prototype
//
// get_own_property and NOTHING else. On the get walk a miss (0) lets the
// lookup continue to Object.prototype, so an unresolved name still ends in
// ReferenceError, and the same hook is what JS_HasProperty consults for
// `'THREE' in window`. A get_property hook here would be wrong: its answer is
// final, which would turn every miss into undefined and cut Object.prototype
// out of the global's chain.
//
// The oracle is ev::globalValue, which resolves in exactly the order a
// compiled read does (builtin ladder, host globals, globalThis own) and never
// walks bronze's Object.prototype — the interpreted realm keeps its own
// hasOwnProperty. Writes are not forwarded: assigning to a bronze-owned name
// creates an own property on the QuickJS global and shadows it from then on,
// the ordinary prototype-shadowing rule, which keeps the write path
// single-realm.

int globalsFallbackGetOwn(JSContext* ctx, JSPropertyDescriptor* desc, JSValueConst,
                          JSAtom prop) {
    // A symbol cannot name a bronze global, and stringifying one throws.
    JSValue key = JS_AtomToValue(ctx, prop);
    if (JS_IsSymbol(key)) {
        JS_FreeValue(ctx, key);
        return 0;
    }
    const char* name = JS_ToCString(ctx, key);
    JS_FreeValue(ctx, key);
    if (!name) return -1;
    ev::GlobalValue g = ev::globalValue(name);
    JS_FreeCString(ctx, name);
    if (!g.found) return 0;
    if (desc) {
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        desc->value = toJs(g.value);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

JSClassExoticMethods g_globalsExotic = {
    /* get_own_property       */ globalsFallbackGetOwn,
    /* get_own_property_names */ nullptr,
    /* delete_property        */ nullptr,
    /* define_own_property    */ nullptr,
    /* has_property           */ nullptr,
    /* get_property           */ nullptr,
    /* set_property           */ nullptr,
};

JSClassDef g_globalsClassDef = {
    /* class_name */ "BronzeGlobals",
    /* finalizer */ nullptr,
    /* gc_mark   */ nullptr,
    /* call      */ nullptr,
    /* exotic    */ &g_globalsExotic,
};

// Splice the fallback between the global object and its prototype. Idempotent:
// a chain that already carries one is left alone.
void installGlobalsFallback() {
    Bridge& b = bridge();
    if (b.globalsClass == 0) {
        JS_NewClassID(JS_GetRuntime(b.ctx), &b.globalsClass);
        JS_NewClass(JS_GetRuntime(b.ctx), b.globalsClass, &g_globalsClassDef);
    }
    JSValue global = JS_GetGlobalObject(b.ctx);
    JSValue proto = JS_GetPrototype(b.ctx, global);
    const bool present =
        JS_IsObject(proto) && JS_GetClassID(proto) == b.globalsClass;
    if (!present) {
        JSValue fallback = JS_NewObjectProtoClass(b.ctx, proto, b.globalsClass);
        if (!JS_IsException(fallback)) JS_SetPrototype(b.ctx, global, fallback);
        JS_FreeValue(b.ctx, fallback);
    }
    JS_FreeValue(b.ctx, proto);
    JS_FreeValue(b.ctx, global);
}

// ---------------------------------------------------------------------------
// The bronze side of a QuickJS object
// ---------------------------------------------------------------------------

// Calling an interpreted function, in one place: the two trap shapes and the
// direct wrapper all funnel through it.
Value callInterpreted(size_t rowIndex, Value thisValue, std::span<const Value> args,
                      bool asConstructor) {
    Bridge& b = bridge();
    if (b.ownerThread != std::thread::id{} && std::this_thread::get_id() != b.ownerThread) {
        return ev::throwError(
            "interpreted function: cross-bridge calls must execute on the main interpreter thread");
    }
    // `args` points at GC-rooted slots the collector updates in place, so
    // reading it per iteration is safe even when a toJs allocates. `thisValue`
    // is a plain copy — embed.h: "re-root it before allocating".
    ev::Persistent thisRoot(thisValue);
    std::vector<JSValue> jsArgs;
    jsArgs.reserve(args.size());
    for (Value a : args) jsArgs.push_back(toJs(a));
    JSValue jsThis = toJs(thisRoot.get());
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
//
// The captured rowIndex cannot go stale despite row reuse: a trap can only
// fire while its proxy is alive, and the row is recycled only after the sweep
// has seen the proxy dead.
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
// Typed arrays: one byte store, a window in each realm
// ---------------------------------------------------------------------------
//
// The old bridge COPIED binary data, because embed's pointer contract made a
// shared buffer impossible — a bronze buffer's bytes lived in the moving heap
// and the address died at the next allocation. That contract now has its
// deliberate exception (embed.h, externalizeArrayBuffer): a buffer's storage
// migrates once into a refcounted host block that never moves, and from then
// on BOTH realms hold real views over the same bytes. A user script's
// `position.array[i] = x` is the write the compiled renderer uploads; the
// copy semantics that silently dropped it are gone.
//
// Identity holds per OBJECT, not merely per byte: the buffer crosses once
// (SharedBuf row), each distinct view crosses once (SharedView row), and a
// round trip in either direction gives back the object that started it.
//
// The copy paths at the bottom remain for exactly one case each way: a
// detached buffer (externalize refuses), and a QuickJS view whose buffer
// bytes cannot be pinned. A DETACH of a shared QuickJS buffer after crossing
// (transfer to a worker, say) frees bytes the bronze side still points at —
// the one lifetime this design does not cover; three.js's editor never
// transfers a geometry array, and a loader that does gets the copy path by
// crossing before it transfers.

struct KindPair {
    bronze::ElementKind kind;
    JSTypedArrayEnum jsKind;
    const char* jsCtor;
};

const KindPair kKinds[] = {
    {ev::elements::Int8, JS_TYPED_ARRAY_INT8, "Int8Array"},
    {ev::elements::Uint8, JS_TYPED_ARRAY_UINT8, "Uint8Array"},
    {ev::elements::Uint8Clamped, JS_TYPED_ARRAY_UINT8C, "Uint8ClampedArray"},
    {ev::elements::Int16, JS_TYPED_ARRAY_INT16, "Int16Array"},
    {ev::elements::Uint16, JS_TYPED_ARRAY_UINT16, "Uint16Array"},
    {ev::elements::Int32, JS_TYPED_ARRAY_INT32, "Int32Array"},
    {ev::elements::Uint32, JS_TYPED_ARRAY_UINT32, "Uint32Array"},
    {ev::elements::Float32, JS_TYPED_ARRAY_FLOAT32, "Float32Array"},
    {ev::elements::Float64, JS_TYPED_ARRAY_FLOAT64, "Float64Array"},
    {ev::elements::Float16, JS_TYPED_ARRAY_FLOAT16, "Float16Array"},
    {ev::elements::BigInt64, JS_TYPED_ARRAY_BIG_INT64, "BigInt64Array"},
    {ev::elements::BigUint64, JS_TYPED_ARRAY_BIG_UINT64, "BigUint64Array"},
};

const KindPair* pairForBronze(bronze::ElementKind kind) {
    for (const KindPair& k : kKinds)
        if (k.kind == kind) return &k;
    return nullptr;
}

const KindPair* pairForJs(int jsKind) {
    for (const KindPair& k : kKinds)
        if (k.jsKind == jsKind) return &k;
    return nullptr;
}

// JS_NewArrayBuffer's free_func for a store the bronze side pinned: the AB's
// death releases the reference the crossing took. Runs inside QuickJS
// collection, and releaseExternalStore is heap-free unless it is the LAST
// reference — which it cannot be while the bronze buffer's own Deferred
// reference is outstanding, and for a store that outlived its bronze buffer
// the deleter is plain free().
void sharedStoreFreeFunc(JSRuntime*, void* opaque, void*) {
    ev::releaseExternalStore(opaque);
}

// The deleter for a store wrapping a QUICKJS buffer's bytes: drop the dup
// that kept the ArrayBuffer (and so its bytes) alive. It runs from bronze's
// deferred-finalizer drain — a plain host stack at the frame boundary —
// where JS_FreeValue is unconditionally safe.
struct JsBytesKeep {
    JSContext* ctx;
    JSValue buffer;
};

void releaseJsBytes(void* user, uint8_t*) {
    auto* keep = static_cast<JsBytesKeep*>(user);
    JS_FreeValue(keep->ctx, keep->buffer);
    delete keep;
}

// The legacy copy, kept for the cases sharing cannot serve.
JSValue typedArrayCopyToJs(Value v) {
    Bridge& b = bridge();
    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
    if (!info) return JS_UNDEFINED;
    const KindPair* pair = pairForBronze(info.elementKind);
    const char* ctorName = pair ? pair->jsCtor : "Uint8Array";
    std::vector<uint8_t> bytes(info.data, info.data + info.byteLength);

    JSValue global = JS_GetGlobalObject(b.ctx);
    JSValue ctor = JS_GetPropertyStr(b.ctx, global, ctorName);
    JS_FreeValue(b.ctx, global);
    JSValue count = JS_NewInt64(
        b.ctx, pair ? info.elementCount : static_cast<int64_t>(bytes.size()));
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

Value typedArrayCopyToBronze(JSValueConst v) {
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

    const KindPair* pair = pairForJs(JS_GetTypedArrayType(v));
    bronze::ElementKind kind = pair ? pair->kind : ev::elements::Uint8;
    const uint32_t perElement =
        pair ? static_cast<uint32_t>(bytesPerElement ? bytesPerElement : 1) : 1;
    ev::Persistent view(ev::createTypedArray(
        kind, static_cast<uint32_t>(bytes.size() / (perElement ? perElement : 1))));
    ev::fillTypedArray(view.get(), std::span<const uint8_t>(bytes));
    return view.get();
}

// The bronze BUFFER's JS twin, created on first crossing: pin the bytes, wrap
// them in a real ArrayBuffer whose death releases the pin. Returns a dup the
// caller owns, or JS_UNDEFINED when the buffer cannot externalize (detached).
JSValue jsBufferFor(Value bronzeBuffer) {
    Bridge& b = bridge();
    if (Crossing* row = rowForBronze(bronzeBuffer)) return JS_DupValue(b.ctx, row->js);
    // Rooted across externalizeArrayBuffer: pinning the BYTES does not pin the
    // header, and the row below must weak-reference the buffer's post-call
    // address, not the bits from before the pin allocated.
    ev::Persistent bufP(bronzeBuffer);
    ev::ExternalBytes ext = ev::externalizeArrayBuffer(bufP.get());
    if (!ext) return JS_UNDEFINED;
    JSValue jsBuf = JS_NewArrayBuffer(b.ctx, ext.data, ext.byteLength, sharedStoreFreeFunc,
                                      ext.store, /*is_shared=*/false);
    if (JS_IsException(jsBuf)) {
        JS_FreeValue(b.ctx, jsBuf);
        JS_GetException(b.ctx);
        ev::releaseExternalStore(ext.store);
        return JS_UNDEFINED;
    }
    addWeakRow(CrossKind::SharedBuf, bufP.get(), jsBuf);
    return jsBuf;
}

// A bronze view going OUT: same store, a JS view over the same window.
JSValue typedArrayToJs(Value v) {
    Bridge& b = bridge();
    if (Crossing* row = rowForBronze(v)) return JS_DupValue(b.ctx, row->js);

    // jsBufferFor's externalize allocates on the moving heap; the view rides
    // rooted so its offset read and the weak row store are of live bits.
    ev::Persistent vP(v);
    ev::TypedArrayInfo info = ev::typedArrayInfo(vP.get());
    if (!info) return JS_UNDEFINED;
    const KindPair* pair = pairForBronze(info.elementKind);
    if (!pair) return typedArrayCopyToJs(vP.get());

    JSValue jsBuf = jsBufferFor(ev::typedArrayBuffer(vP.get()));
    if (JS_IsUndefined(jsBuf)) return typedArrayCopyToJs(vP.get());

    JSValue argv[3] = {jsBuf, JS_NewInt64(b.ctx, ev::typedArrayByteOffset(vP.get())),
                       JS_NewInt64(b.ctx, info.elementCount)};
    JSValue jsView = JS_NewTypedArray(b.ctx, 3, argv, pair->jsKind);
    JS_FreeValue(b.ctx, argv[0]);
    JS_FreeValue(b.ctx, argv[1]);
    JS_FreeValue(b.ctx, argv[2]);
    if (JS_IsException(jsView)) {
        JS_FreeValue(b.ctx, jsView);
        JS_GetException(b.ctx);
        return typedArrayCopyToJs(vP.get());
    }
    addWeakRow(CrossKind::SharedView, vP.get(), jsView);
    return jsView;
}

// A QuickJS view coming IN: pin its buffer's bytes under a bronze external
// buffer, and hand back a bronze view over the same window.
Value typedArrayToBronze(JSValueConst v) {
    Bridge& b = bridge();
    if (Crossing* row = rowForJs(v)) {
        Value view = derefWeakRef(row->bronze.get());
        if (ev::isObject(view)) return view;
    }

    const KindPair* pair = pairForJs(JS_GetTypedArrayType(v));
    if (!pair) return typedArrayCopyToBronze(v);

    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue jsBuf = JS_GetTypedArrayBuffer(b.ctx, v, &byteOffset, &byteLength,
                                           &bytesPerElement);
    if (JS_IsException(jsBuf)) {
        JS_FreeValue(b.ctx, jsBuf);
        JS_GetException(b.ctx);
        return ev::undefined();
    }

    // Both halves ride in Persistents from creation: addWeakRow's WeakRef
    // construction allocates, and the buffer is still needed for the view
    // after its row exists — as the view is still needed for the return
    // after its own.
    ev::Persistent bronzeBuf;
    if (Crossing* brow = rowForJs(jsBuf)) {
        bronzeBuf.set(derefWeakRef(brow->bronze.get()));
    }
    if (!ev::isObject(bronzeBuf.get())) {
        size_t bufLen = 0;
        uint8_t* data = JS_GetArrayBuffer(b.ctx, &bufLen, jsBuf);
        if (!data) {
            JS_FreeValue(b.ctx, jsBuf);
            JS_GetException(b.ctx);
            return typedArrayCopyToBronze(v);
        }
        // The store's reference to the BUFFER is what keeps these bytes
        // valid; it drops from bronze's deferred drain when the bronze
        // buffer dies.
        auto* keep = new JsBytesKeep{b.ctx, JS_DupValue(b.ctx, jsBuf)};
        bronzeBuf.set(ev::createExternalArrayBuffer(data, static_cast<uint32_t>(bufLen),
                                                    releaseJsBytes, keep));
        if (!ev::isObject(bronzeBuf.get())) {
            JS_FreeValue(b.ctx, keep->buffer);
            delete keep;
            JS_FreeValue(b.ctx, jsBuf);
            return typedArrayCopyToBronze(v);
        }
        addWeakRow(CrossKind::SharedBuf, bronzeBuf.get(), jsBuf);
    }
    JS_FreeValue(b.ctx, jsBuf);

    const uint32_t per = static_cast<uint32_t>(bytesPerElement ? bytesPerElement : 1);
    ev::Persistent view(ev::createTypedArrayView(pair->kind, bronzeBuf.get(),
                                                 static_cast<uint32_t>(byteOffset),
                                                 static_cast<uint32_t>(byteLength / per)));
    if (!ev::isObject(view.get())) return typedArrayCopyToBronze(v);
    addWeakRow(CrossKind::SharedView, view.get(), v);
    return view.get();
}

// ---------------------------------------------------------------------------
// The conversion pair
// ---------------------------------------------------------------------------

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
    // Rooted from here on: every probe below may allocate on the moving heap
    // (hostElementOf reads a brand via getProperty), and a raw `v` surviving
    // one of those is a pre-collection address. This was the editor's Play
    // crash: the flip landed between a globalValue read and the row store,
    // and the fresh row then ROOTED dangling bits — rooting stale bits does
    // not resurrect them.
    ev::Persistent vP(v);
    if (ev::isTypedArray(vP.get())) return typedArrayToJs(vP.get());
    if (ev::isArrayBuffer(vP.get())) {
        JSValue jsBuf = jsBufferFor(vP.get());
        if (!JS_IsUndefined(jsBuf)) return jsBuf;
        // detached: fall through to the wrapper, which is at least identity
    }
    // An ENGINE object is not wrapped in either direction: both realms already
    // wrap the same dom::Element, so the crossing resolves to the other
    // realm's existing wrapper for it. This is the README's rule ("Engine
    // objects are shared") reaching the bridge — and it is not an optimisation
    // but the difference between working and not: appendChild on either side
    // unwraps to a dom::Node, and a generic forwarding wrapper is not one.
    if (dom::Element* el = hostElementOf(vP.get()))
        return js::DomBindings::wrapElement(b.ctx, el);
    if (Crossing* row = rowForBronze(vP.get())) {
        if (row->kind == CrossKind::BronzeOut) return JS_DupValue(b.ctx, row->js);
        // A proxy for a JS object, going home: the original.
        return JS_DupValue(b.ctx, row->js);
    }
    return makeJsRef(vP.get());
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
    if (Crossing* row = rowForJs(v)) {
        Value wrapper = derefWeakRef(row->bronze.get());
        if (ev::isObject(wrapper)) return wrapper;
        // The wrapper died between sweeps; fall through and cross afresh.
    }

    // A typed-array view shares its store rather than wrapping or copying —
    // element access on one is not a property read the traps could serve, and
    // a copy is a divergence the first write exposes.
    if (JS_GetTypedArrayType(v) >= 0) return typedArrayToBronze(v);

    const size_t index = takeRow();
    // The wrapper rides in a Persistent from birth: makeWeakRef CONSTRUCTS a
    // WeakRef, and that allocation is allowed to move the wrapper — the raw
    // Value this used to return was stale bits whenever the construction
    // collected, which the editor's Play burst hit within a frame.
    ev::Persistent wrapper(JS_IsFunction(b.ctx, v) ? makeBronzeFunction(index)
                                                   : makeBronzeObject(index));
    // Fill the reserved row in place: the traps captured `index`, so the row
    // must be THIS one.
    Crossing& row = *b.rows[index];
    row.kind = CrossKind::JsIn;
    row.deadPending = false;
    row.bronze.set(makeWeakRef(wrapper.get()));
    row.js = JS_DupValue(b.ctx, v);
    b.byJs[JS_VALUE_GET_PTR(v)] = index;
    if (b.byBronzeEpoch != ev::relocationEpoch()) rebuildByBronze();
    b.byBronze[ev::toBits(wrapper.get())] = index;
    return wrapper.get();
}

// ---------------------------------------------------------------------------
// Dynamic code (27.3.1.1 and 19.2.1), performed by the interpreter
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
    if (b.ownerThread != std::thread::id{} && std::this_thread::get_id() != b.ownerThread) {
        return ev::throwError(
            "new Function: dynamic compilation must execute on the main interpreter thread");
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
    // Drain any microtasks/promise jobs spawned during compilation
    while (JS_IsJobPending(JS_GetRuntime(b.ctx))) {
        JSContext* pctx = nullptr;
        if (JS_ExecutePendingJob(JS_GetRuntime(b.ctx), &pctx) < 0) {
            JSValue e = JS_GetException(pctx ? pctx : b.ctx);
            JS_FreeValue(pctx ? pctx : b.ctx, e);
        }
    }
    Value out = toBronze(fn);
    JS_FreeValue(b.ctx, fn);
    return out;
}

// A compiled `eval(src)`, evaluated by the page's realm in GLOBAL scope —
// which is the semantics bronze's provided `eval` promises for both the
// direct and the indirect spelling (an AOT frame has no local scope to hand
// over, and the lowering warned at any direct call site). The runtime has
// already performed 19.2.1 step 2, so `source` is always a string.
Value dynamicEval(Value source) {
    Bridge& b = bridge();
    if (!b.ctx) {
        return ev::throwError(
            "eval: no interpreter realm to compile in (the bridge is not installed)");
    }
    if (b.ownerThread != std::thread::id{} && std::this_thread::get_id() != b.ownerThread) {
        return ev::throwError(
            "eval: dynamic evaluation must execute on the main interpreter thread");
    }
    const std::string text = ev::toUtf8(source);
    JSValue r = JS_Eval(b.ctx, text.c_str(), text.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JS_FreeValue(b.ctx, r);
        return ev::throwError("eval: " + describeJsException(b.ctx));
    }
    // Drain any microtasks/promise jobs spawned during eval
    while (JS_IsJobPending(JS_GetRuntime(b.ctx))) {
        JSContext* pctx = nullptr;
        if (JS_ExecutePendingJob(JS_GetRuntime(b.ctx), &pctx) < 0) {
            JSValue e = JS_GetException(pctx ? pctx : b.ctx);
            JS_FreeValue(pctx ? pctx : b.ctx, e);
        }
    }
    Value out = toBronze(r);
    JS_FreeValue(b.ctx, r);
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
    b.ownerThread = std::this_thread::get_id();
    if (b.refClass == 0) {
        JS_NewClassID(JS_GetRuntime(b.ctx), &b.refClass);
        JS_NewClass(JS_GetRuntime(b.ctx), b.refClass, &g_refClassDef);
    }
    ev::setDynamicFunctionHook(dynamicFunction);
    ev::setDynamicEvalHook(dynamicEval);
    installGlobalsFallback();
    LOG_INFO("bronze_host: interpreter bridge installed (compiled `new Function` and "
             "`eval` compile in the app's QuickJS realm, whose global lookups fall "
             "back to the compiled realm's globals)");
}

Value bridgeJsGlobal(const char* name) {
    Bridge& b = bridge();
    if (!b.ctx) return ev::undefined();
    if (b.ownerThread != std::thread::id{} && std::this_thread::get_id() != b.ownerThread) {
        return ev::undefined();
    }
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

void sweepInterpBridge() {
    Bridge& b = bridge();
    if (!b.ctx) return;
    if (b.ownerThread != std::thread::id{} && std::this_thread::get_id() != b.ownerThread) {
        return;
    }

    // 1. Rows whose BronzeOut wrapper finalized since the last sweep. The
    //    vector is swapped out first: freeing a weak row's dup below can run
    //    QuickJS collection, whose finalizers push NEW indices — those wait
    //    for the next frame.
    std::vector<size_t> dead;
    dead.swap(b.pendingDead);
    for (size_t index : dead) {
        if (index >= b.rows.size()) continue;
        Crossing& row = *b.rows[index];
        if (row.kind == CrossKind::BronzeOut && row.deadPending) {
            freeRow(index, /*freeOwnedJs=*/false);
        }
    }

    // 2. Weak rows whose bronze-side wrapper the collector has proved dead.
    //    Rows can only die at a collection, so a quiet epoch means nothing to
    //    scan.
    static uint64_t lastSweepEpoch = ~0ULL;
    const uint64_t epoch = ev::relocationEpoch();
    if (epoch != lastSweepEpoch) {
        lastSweepEpoch = epoch;
        for (size_t i = 0; i < b.rows.size(); ++i) {
            Crossing& row = *b.rows[i];
            if (row.kind != CrossKind::JsIn && row.kind != CrossKind::SharedBuf &&
                row.kind != CrossKind::SharedView) {
                continue;
            }
            if (!ev::isObject(derefWeakRef(row.bronze.get()))) {
                freeRow(i, /*freeOwnedJs=*/true);
            }
        }
    }
}

void resetInterpBridge() {
    Bridge& b = bridge();
    for (size_t i = 0; i < b.rows.size(); ++i) {
        Crossing& row = *b.rows[i];
        // BronzeOut wrappers are not owned here; they die with the realm and
        // their finalizers find rows already reset.
        const bool ownsJs = row.kind == CrossKind::JsIn ||
                            row.kind == CrossKind::SharedBuf ||
                            row.kind == CrossKind::SharedView;
        if (ownsJs && b.ctx) JS_FreeValue(b.ctx, row.js);
        row.kind = CrossKind::Free;
        row.deadPending = false;
        row.bronze.set(ev::undefined());
        row.js = JS_UNDEFINED;
    }
    b.rows.clear();
    b.freeRows.clear();
    b.pendingDead.clear();
    b.byJs.clear();
    b.byBronze.clear();
    b.byBronzeEpoch = ~0ULL;
}

}  // namespace bro::bronze_host

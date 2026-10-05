// The minimal DOM a bronze-compiled three.js app touches: document (create a
// canvas, append it), the canvas element itself (size, style, getContext),
// window (sizes, DPR, resize listeners), and requestAnimationFrame driven by
// Engine::onFrame. Everything else the web platform offers is deliberately
// absent — an unknown createElement tag is a named refusal, not a stub that
// fails later.
//
// What is HERE: the frame seam (hostFrame), document, the canvas object, and
// installWebHostGlobals. What moved out, and where to look for it: the
// `window` object to dom_window.cpp, localStorage to dom_storage.cpp, the gamepad surface and the navigator
// base object to dom_gamepad.cpp, navigator's clipboard and getBattery to
// host_navigator.cpp, the element surface to host_element*.cpp.
//
// bronze already provides `console` from its own runtime (rt_print.cpp), so
// no console is registered here — duplicating it would shadow the builtin
// ladder for nothing (host globals lose to builtins anyway; see
// runtime/host_globals.h).
//
// LIFETIME MODEL: one process-lived HostState (never freed — the
// Engine::onFrame convention: callbacks register once and are never
// unregistered, so their captures must outlive the loop). Bronze Values held
// across frames (canvas objects, rAF callbacks, listeners) live in
// embed::Persistent slots; dom::Element pointers are owned by the Document
// and outlive the compiled program's run.

#include "bronze_host/bronze_host.h"
#include "bronze_host/webgl_internal.h"
#include "bronze_host/host_canvas2d.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/eval.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif
#include "bronze_host/host_headless.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_range.h"
#include "bronze_host/host_selection.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_rejection_events.h"
#include "bronze_host/host_template.h"
#include "bronze_host/host_window_open.h"
#include "bronze_host/host_intl.h"
#include "bronze_host/host_dom_events_types.h"
#include "bronze_host/host_storage.h"
#include "bronze_host/host_web_animations.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_target.h"
#include "css/parser.h"
#include "util/log.h"
#include <unordered_set>

#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct RafEntry {
    int32_t id;
    dom::Document* doc = nullptr;
    ev::Persistent fn;
};

struct HostState {
    engine::Engine* engine = nullptr;
    std::vector<RafEntry> rafPending;
    std::unordered_set<int32_t> rafActiveBatchCancelled;
    int32_t nextRafId = 1;
    // The rAF/performance clock: accumulated scaled-frame deltas, so
    // timestamps here advance exactly as the engine's own rAF timestamps do
    // (paused while bro.time is paused, virtual under headless advanceTime).
    double clockMs = 0.0;
};

HostState* g_host = nullptr;

// ---------------------------------------------------------------------------
// requestAnimationFrame
// ---------------------------------------------------------------------------

// REENTRANCY: the pending list is MOVED OUT before any callback runs,
// so a callback that registers another rAF appends to the fresh list (fires next frame),
// and a cancelAnimationFrame from inside a callback cancels matching entries in both
// not-yet-moved future entries and the currently executing batch.
void fireAnimationFrames() {
    if (g_host->rafPending.empty()) return;

    std::vector<RafEntry> current = std::move(g_host->rafPending);
    g_host->rafPending.clear();
    g_host->rafActiveBatchCancelled.clear();

    // A hidden system panel's callbacks wait, in order, ahead of anything
    // registered this frame (Engine::isSystemDocumentHidden).
    std::vector<RafEntry> waiting;
    for (RafEntry& entry : current) {
        if (g_host->rafActiveBatchCancelled.count(entry.id)) {
            continue;
        }
        if (g_host->engine && g_host->engine->isSystemDocumentHidden(entry.doc)) {
            waiting.push_back(std::move(entry));
            continue;
        }
        dom::Document* targetDoc = entry.doc;
        dom::Document* prevDoc = currentHostDocument();
        bool swapDoc = (targetDoc && targetDoc != prevDoc);

        ev::GlobalValue docG = ev::globalValue("document");
        // A Persistent, not a raw Value: the callback may allocate enough to
        // move the heap, and the restore must name the document's CURRENT
        // address; `globalThis` is re-read after the call for the same reason.
        ev::Persistent prevDocVal(docG.found ? docG.value : ev::null());

        if (swapDoc) {
            enterRealmScope(scopeIdForDocument(targetDoc));
            setCurrentHostDocument(targetDoc);
            ev::Persistent subDocVal(hostDocumentValue(targetDoc));
            ev::registerGlobal("document", subDocVal.get());
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (gt.found && ev::isObject(gt.value)) {
                ev::setProperty(gt.value, "document", subDocVal.get());
            }
        }

        Value ts = ev::fromDouble(g_host->clockMs);
        ev::CallResult r = ev::call(entry.fn.get(), ev::undefined(),
                                    std::span<const Value>(&ts, 1));
        // The thrown value outlives the document swap back, which allocates.
        ev::Persistent thrown(r.thrown ? r.value : ev::undefined());

        if (swapDoc) {
            if (!ev::isNull(prevDocVal.get())) {
                ev::registerGlobal("document", prevDocVal.get());
                ev::GlobalValue gt = ev::globalValue("globalThis");
                if (gt.found && ev::isObject(gt.value)) {
                    ev::setProperty(gt.value, "document", prevDocVal.get());
                }
            }
            setCurrentHostDocument(prevDoc);
            exitRealmScope();
        }

        if (r.thrown) reportBronzeError("requestAnimationFrame", thrown.get());
    }
    if (!waiting.empty()) {
        std::vector<RafEntry> carried;
        for (RafEntry& entry : waiting) {
            if (!g_host->rafActiveBatchCancelled.count(entry.id)) carried.push_back(std::move(entry));
        }
        for (RafEntry& entry : g_host->rafPending) carried.push_back(std::move(entry));
        g_host->rafPending = std::move(carried);
    }
    g_host->rafActiveBatchCancelled.clear();
}

// ---------------------------------------------------------------------------
// The frame seam
// ---------------------------------------------------------------------------

// Everything this layer does per frame, in one place, fired from
// Engine::onFrame — which the engine calls at the point its own rAF fires
// (engine_frame.cpp step 3a, headless_api.cpp's advanceTime step, and the
// server tick), under rAF's pause gate, with the delta the scaled clock
// actually advanced by.
//
// THE ORDER, and what each position is answering to:
//
//  1. A leftover drain. Any task that enqueued a bronze job after we returned
//     last frame — e.g. a listener dispatched during input processing — is
//     drained here before advancing time and firing frame callbacks.
//     Draining here costs a queue check when there is nothing to do and ensures
//     jobs are processed without waiting an extra frame.
//     It matters because an unhandled rejection is only REPORTED at quiescence:
//     nobody ever hears about.
//
//  2. The clock. Advanced before anything reads it, so a timer deadline, an
//     rAF timestamp and performance.now() inside one frame all agree.
//
//  3. Host tasks — image loads. Before rAF, because that is where the web
//     runs a load event relative to the rendering steps, and because it lets
//     a texture that finished decoding be uploaded by the very frame that
//     learns about it rather than the next one. brokit's polled completions
//     (fetch, WebSocket, sockets, fs.watch; host_brokit.cpp) are host tasks
//     of the same kind and are pumped in the same position.
//
//  4. Timers, then 5. rAF. The order bro's own loop uses (timers_->tick at
//     step 2, fireAnimationFrames at step 3a).
//
//  6. The microtask checkpoint. AFTER rAF, not before: an rAF callback is the
//     main producer of promise jobs in a render loop — three.js's own
//     `renderer.setAnimationLoop` body, every `await` an app puts in its frame
//     function — and draining before it would leave every one of those jobs
//     queued until the next frame. That is not merely late: each frame would
//     run the PREVIOUS frame's continuations against this frame's state, and a
//     rejection thrown in the last rAF before shutdown would never be reported
//     at all, because quiescence would never be reached again.
//
//  6b. The observer pass — ResizeObserver, then IntersectionObserver
//     (js/observers.js, over host_observer_hooks.cpp). After the checkpoint,
//     which is where the web's "update the rendering" puts them: a mutation
//     made in an rAF callback has been reported (MutationObserver delivers
//     from a microtask) before the box it changed is measured, so a resize
//     entry describes the box as it ended up rather than mid-edit. The pass
//     reads geometry, and a geometry read lays the document out first, so
//     "after layout" is what it measures. Its own callbacks' promise jobs
//     drain at 6c.
//
//  7a. The detached-tree sweep (host_node_sweep.cpp). Last, after every
//     callback and checkpoint of the frame: its pass takes wrappers off the
//     roots for the length of one collection, which is only safe with no
//     program code left to run, and the WeakRefs it reads groups back through
//     rely on the checkpoint above having released what they kept.
//
// NOT here: broaudio's per-frame tick (audioFramePump below). Audio plays in
// real time whether or not bro.time is paused, so its automation, `onended`
// and mic chunks ride the engine's ungated frame pump rather than this
// pause-gated seam.
void hostFrame(double dtMs) {
    drainMicrotasksAndLocalFetches();                    // 1
    g_host->clockMs += dtMs;                             // 2
    drainHostTasks();                                    // 3
    pumpBrokitTicks();                                   // 3b
    drainWorkerMessages();                               // 3c
    pollNet();                                           // 3d
    drainSteamEvents();                                  // 3e
    drainHostWindowMessages();                           // 3f
    fireHostTimers(g_host->clockMs);                     // 4
    fireAnimationFrames();                               // 5
    drainMicrotasksAndLocalFetches(/*always=*/true);     // 6
    fireHostObserverFrame();                             // 6b
    deliverWebAnimationFinishEvents();
    drainMicrotasksAndLocalFetches(/*always=*/true);     // 6c
    hostDomSweepFrame(dtMs);                             // 7a
    hostNotifyIdleFrame(dtMs);                           // 7
    flushHostStorage();
}

// ---------------------------------------------------------------------------
// Free-function globals
// ---------------------------------------------------------------------------

Value makeRequestAnimationFrame() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            Value fn = argAt(a, 0);
            if (!ev::isFunction(fn)) {
                return ev::throwTypeError(
                    "requestAnimationFrame: first argument must be a function");
            }
            int32_t id = g_host->nextRafId++;
            dom::Document* curDoc = currentHostDocument() ? currentHostDocument() : (g_host->engine ? g_host->engine->document() : nullptr);
            g_host->rafPending.push_back({id, curDoc, ev::Persistent(fn)});
            return ev::fromDouble(id);
        },
        1);
}

Value makeCancelAnimationFrame() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            int32_t id = i32At(a, 0);
            auto& pending = g_host->rafPending;
            for (auto it = pending.begin(); it != pending.end(); ++it) {
                if (it->id == id) {
                    pending.erase(it);
                    break;
                }
            }
            g_host->rafActiveBatchCancelled.insert(id);
            return ev::undefined();
        },
        1);
}

}  // namespace

void clearHostAnimationFramesForDocument(dom::Document* doc) {
    if (!g_host || !doc) return;
    auto it = std::remove_if(g_host->rafPending.begin(), g_host->rafPending.end(),
                             [doc](const RafEntry& e) { return e.doc == doc; });
    g_host->rafPending.erase(it, g_host->rafPending.end());
}

// The same surface the `document` global has — createElement, the queries, the
// element accessors, the event-target delegation — bound to `doc` instead of to
// whatever the engine is showing. host_parser.cpp hands DOMParser results
// through here, which is why it exists at all: the builder itself is private to


Value makeBrandConstructor(const char* name) {
    std::string msg = std::string("bronze host ") + name +
                      ": an instanceof brand only, not constructible";
    return ev::makeFunction(
        [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); }, 0);
}

// The text a thrown value is reported with. bronze attaches NO `stack` and no
// source position to an Error it raises (runtime/exception.h: "bronze has no
// stack to print, which is a deliberate divergence from node"; the JIT's
// `retainSource` keeps source text for Function.prototype.toString only), so
// the best a report can carry is what the value itself says:
//   - `stack`, when a program or a host set one (a non-empty string);
//   - else `Name: message` for an Error-shaped object (an object with a
//     string `message`), the shape console.log prints;
//   - else, for any other object, its JSON — a `{code, reason}` thrown by a
//     native or a rejected event descriptor reads as itself rather than as
//     "[object]", which named nothing;
//   - else ToString of the primitive.
// The Error's own fields are read through embed property reads — the throw
// was already caught, so running a getter here is safe — and the value is
// rooted first, because every one of those reads may allocate.
std::string thrownValueText(Value thrown) {
    if (!ev::isObject(thrown)) return ev::toUtf8(thrown);
    ev::Persistent root(thrown);
    Value stackV = ev::getProperty(root.get(), "stack");
    if (ev::isString(stackV)) {
        std::string stack = ev::toUtf8(stackV);
        if (!stack.empty()) return stack;
    }
    Value msgV = ev::getProperty(root.get(), "message");
    if (ev::isString(msgV)) {
        // Copied out before the `name` read, whose getter may allocate.
        std::string msg = ev::toUtf8(msgV);
        Value nameV = ev::getProperty(root.get(), "name");
        std::string name = ev::isString(nameV) ? ev::toUtf8(nameV) : std::string("Error");
        if (name.empty()) return msg;
        return msg.empty() ? name : name + ": " + msg;
    }
    ev::GlobalValue json = ev::globalValue("JSON");
    if (json.found) {
        ev::Persistent jsonRoot(json.value);
        Value stringify = ev::getProperty(jsonRoot.get(), "stringify");
        if (ev::isFunction(stringify)) {
            Value arg = root.get();
            auto r = ev::call(stringify, jsonRoot.get(), std::span<const Value>(&arg, 1));
            if (!r.thrown && ev::isString(r.value)) return ev::toUtf8(r.value);
        }
    }
    // An object JSON cannot print (a cycle, a BigInt field): toUtf8 of an
    // object is a hard error in the embed API, not a ToString, so the text
    // is the one Object.prototype.toString would give.
    return "[object Object]";
}

// Where an exception out of compiled code ends up: the window's `error`
// event first (host_error_events.cpp), then the log stream unless a handler
// cancelled it.
void reportBronzeError(const char* origin, Value thrown) {
    ev::Persistent root(thrown);
    if (hostDispatchUncaughtError(root.get())) return;
    LOG_ERROR("[bronze:%s] uncaught %s", origin, thrownValueText(root.get()).c_str());
}

// Zero before the first frame, which is what a program's top level sees. It is
// an ELAPSED clock, not a wall clock: three.js's Clock only ever subtracts two
// readings, and an origin of zero keeps a headless run's output free of the one
// number that would differ every time it ran.
double hostClockMs() { return g_host ? g_host->clockMs : 0.0; }

engine::Engine* hostEngine() { return g_host ? g_host->engine : nullptr; }



// Identity — the value the program already holds for `el`, so
// `event.target === canvas` is true inside a compiled listener. It asks the
// element registry (host_element.cpp), which is every element this layer ever
// wrapped, not just the canvases: a UI tests target identity on every element
// it built, not only on the one it draws into.
Value hostValueForElement(dom::Element* el) {
    if (!g_host || !el) return ev::undefined();
    return hostElementValue(el);
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------
static void snapshotBaselineGlobalProps();

void installWebHostGlobals(engine::Engine& engine) {
    if (g_host) {
        LOG_WARN("bronze_host: installWebHostGlobals called twice; ignoring");
        return;
    }
    // Every registerGlobal below, and in the install* calls at the foot, is
    // the whole contract: registeredHostGlobals() reads the names back off
    // bronze's registry after this returns, and that is the list every
    // compile is given (eval_jit.cpp, host_worker.cpp, and `bro-headless
    // --print-host-globals` for an ahead-of-time `bronze build`). There is
    // no manifest to keep in step. registerGlobal roots each value for the
    // life of the process.
    //
    // Never freed — see the lifetime note at the top of this file.
    g_host = new HostState();
    g_host->engine = &engine;
    if (engine.document()) {
        engine.document()->setElementClonedCallback(&fireElementCloned);
    }

    // Install dynamic evaluation and function hooks (eval, new Function)
    // into bronze before anything the compiled program can run.
    installDynamicHooks(engine);

    // The frame hook, registered exactly once (Engine::onFrame callbacks are
    // never unregistered). It fires at the point the engine's own rAF fires,
    // with rAF's pause semantics, in every display mode. hostFrame above owns
    // the ordering, drain included, and says why each step sits where it does.
    engine.onFrame([](double dtMs) { hostFrame(dtMs); });

#if BRO_WITH_AUDIO
    // broaudio's once-per-frame host call (broaudio/api.h): drainMicChunks
    // delivers bro.mic chunks and runs tickAsyncJobs, which evaluates
    // AudioParam automation into the playing sources, fires a finished
    // source's `onended`, and settles createClipFromFileAsync. Without it
    // automation freezes and `onended` never fires. An engine frame pump,
    // not the pause-gated frame seam: audio keeps playing while bro.time is
    // paused, so its automation and end events keep coming too. The
    // callbacks' promise jobs drain here, in the frame that produced them.
    engine.addFramePump([] {
        broaudio::api::drainMicChunks();
        if (ev::microtasksPending()) ev::drainMicrotasks();
    });
#endif

    // Install HTML interfaces BEFORE document is created:
    installHtmlInterfaces();

    // HTML's `pattern` attribute is an ECMAScript regular expression by
    // definition, so the constraint layer (src/layout/form_validation.cpp)
    // asks the host to test one rather than carrying a second, differing
    // regex engine of its own. This is where it is handed the tester; until
    // it is, every `pattern` constraint silently matches everything.
    installHostPatternTester();

    {
        // Null: the global follows the engine's current document rather than
        // naming one. documentFor() above has the reason.
        // Registered (rooted) straight away; the globalThis copies are read
        // back from the registry, since the constructor lookup and each
        // setProperty may allocate.
        ev::registerGlobal("document", makeDocumentValue(nullptr));
        ev::registerGlobal("Document", documentHostClass().constructor());
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::Persistent global(gt.value);
            ev::setProperty(global.get(), "document", ev::globalValue("document").value);
            ev::setProperty(global.get(), "Document", ev::globalValue("Document").value);
        }
    }
    // window, self, top, parent and the window-shaped surface (dom_window.cpp).
    installWindowGlobal(g_host->engine);
    {
        // Registered (rooted) first, then read back from the registry for the
        // globalThis copy: the globalThis lookup may allocate, which a raw
        // local would not survive.
        auto registerAndSet = [](const char* name, Value made) {
            ev::registerGlobal(name, made);
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (!gt.found || !ev::isObject(gt.value)) return;
            ev::Persistent global(gt.value);
            ev::Persistent v(ev::globalValue(name).value);
            ev::setProperty(global.get(), name, v.get());
        };
        registerAndSet("requestAnimationFrame", makeRequestAnimationFrame());
        registerAndSet("cancelAnimationFrame", makeCancelAnimationFrame());
        registerAndSet("performance", makePerformanceValue());
    }
    installWebGLGlobals();

    // The families that own their own files, each registering the names
    // the manifest lists for it, in the manifest's order.
    installTimerGlobals();
    installPlatformGlobals();
    // AFTER installPlatformGlobals, which is where installElementGlobals runs:
    // `Image` is an element class and chains its prototype onto Element's, so
    // Element's has to exist first (host_element_image.cpp).
    installImageGlobal();
    installParserGlobal();
    installCssNamespace();

    installNavigatorGlobal();
    // HTMLCanvasElement and HTMLImageElement are installed as real classes
    // via installHtmlInterfaces() / installImageGlobal().
    installTouchGlobals();
    installVendorGlobals();
    installBrokitGlobals(engine);
    // The UI event classes (js/events.js) extend the `Event` brokit just
    // installed.
    installEventsModule();
    // window -> Window.prototype -> EventTarget.prototype -> Object.prototype
    // (brokit's EventTarget is installed by now), so String(window) and
    // window instanceof Window work.
    installGlobalPrototype("Window");
    installDomEventTypes();
    // The math classes BEFORE the roots: bro.math aliases the SpatialHash3D /
    // Rng / Smoother constructors, so they have to exist when the root is
    // assembled (host_math_funcs.cpp reads them off their HostClass).
    installMathGlobals();
    // The `bro` / `__bro` roots, the natives under `__bro_native`, every
    // sibling library's API over them (bro.mesh, bro.lm, bro.stt, bro.tensor,
    // AudioContext, AI, ... — host_sibling_apis.cpp, ONCE each), and
    // js/bro_core.js on top (host_bro_root.cpp). Nothing before this point
    // registers `bro`; a later `bro.*` namespace mounts onto the object this
    // creates, and nothing after this point may call a sibling's install*().
    installBroRoots(engine);
    // bro.image.gpu (js/image_gpu.js) mounts onto the `bro.image` the roots
    // just built; it reads nothing else at load, so it goes here rather than
    // with the other compiled modules below.
    installImageGpuModule();
    // bro's own compiled JavaScript (host_js_modules.cpp), after the roots
    // and the siblings: every name a module lists in js/module.globals must
    // already be registered when its entry runs (the roots, and the classes
    // an earlier module of this family lifted), and a sibling class such as
    // `Mesh` is read off globalThis at the point of use, never at load.
#if BRO_WITH_3D
    installPhysicsModule();
    installTerrainModule();
    installClipmapModule();
    installTileWorldModule();
    installLightingModule();
    installGizmoModule();
    installAnimationModule();
    installSceneModule();
    installImpostorModule();
#endif
    // The sync factory BEFORE js/net.js, which mounts `bro.net.sync` from
    // `globalThis.__bro_net_sync` at its own load; the factory reads nothing
    // at load and binds to the primitives only when called.
#if BRO_WITH_NET
    installNetSyncModule();
    installNetModule();
#endif
#if BRO_WITH_DIFFUSION && BRO_WITH_LM
    installMotionModule();
#endif
    // bro.server in every mode: bro-server's loop, and in a windowed bro the
    // members a script that also runs as a hosted server reads.
    installServerModule();
    // Every feature namespace is mounted now: stamp `available: true` on the
    // compiled-in ones (the stubs already say false).
    markAvailableNamespaces();
    // observers.js reads queueMicrotask, performance and getComputedStyle
    // off globalThis at the point of use.
    installObserversModule();
    installHeadlessGlobals(engine);
    installPlatformExtensions(engine);
    installRangeGlobals();
    installSelectionGlobals();
    installIntlGlobals();
    installWebAnimationGlobals();
    installVideoGlobals();
    // unhandledrejection / rejectionhandled at the window: bronze's
    // end-of-drain report goes to the page from here on, not to stderr.
    installMainThreadRejectionTracking();
    // Escape → close request → the topmost modal dialog's cancel/close.
    installDialogHooks(engine);

    if (engine.installHostBindings()) {
        engine.installHostBindings()(engine);
    }

    snapshotBaselineGlobalProps();
}

static std::vector<std::string> s_baselineGlobalProps;

// Object.getOwnPropertyNames(globalThis), as strings. Every heap value is
// rooted across the calls and element reads, each of which may allocate.
static bool globalOwnPropertyNames(std::vector<std::string>& out) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return false;
    ev::Persistent global(gt.value);
    ev::Persistent objCtor(ev::globalValue("Object").value);
    ev::Persistent gopn(ev::getProperty(objCtor.get(), "getOwnPropertyNames"));
    if (!ev::isFunction(gopn.get())) return false;
    Value arg = global.get();
    ev::CallResult res = ev::call(gopn.get(), objCtor.get(), std::span<const Value>(&arg, 1));
    if (res.thrown) return false;
    ev::Persistent names(res.value);
    int len = satCast<int>(ev::toDouble(ev::getProperty(names.get(), "length")));
    out.clear();
    out.reserve(len > 0 ? static_cast<size_t>(len) : 0);
    for (int i = 0; i < len; ++i) {
        out.push_back(ev::toUtf8(ev::getElement(names.get(), i)));
    }
    return true;
}

// Reflect.deleteProperty(globalThis, key), then the undefined assignment the
// reload path has always made after it.
static void clearGlobalProp(const std::string& key) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    ev::Persistent global(gt.value);
    ev::Persistent reflect(ev::globalValue("Reflect").value);
    if (ev::isObject(reflect.get())) {
        ev::Persistent del(ev::getProperty(reflect.get(), "deleteProperty"));
        if (ev::isFunction(del.get())) {
            ev::Persistent k(ev::fromUtf8(key));
            Value args[2] = {global.get(), k.get()};
            ev::call(del.get(), reflect.get(), args);
        }
    }
    ev::setProperty(global.get(), key, ev::undefined());
}

static void snapshotBaselineGlobalProps() {
    initRealmScopeBaseline();
    std::vector<std::string> names;
    if (globalOwnPropertyNames(names)) s_baselineGlobalProps = std::move(names);
}

void resetGlobalExpandos() {
    resetAllRealmScopes();
    setCurrentHostDocument(nullptr);
    if (g_host) {
        g_host->rafPending.clear();
    }
    clearWindowListeners();
    resetWindowHostOpenState();
    cleanupSteamBindings();
    std::vector<std::string> names;
    if (globalOwnPropertyNames(names)) {
        std::unordered_set<std::string> baseline(s_baselineGlobalProps.begin(), s_baselineGlobalProps.end());
        for (const auto& key : names) {
            if (baseline.find(key) == baseline.end()) clearGlobalProp(key);
        }
    }
    clearGlobalProp("__reloadCanary");
    clearGlobalProp("__afterReloadCall");
}

bool isWebHostGlobalsInstalled() {
    return g_host != nullptr;
}

std::vector<std::string> registeredHostGlobals() {
    return ev::hostGlobalNames();
}

// A hidden system panel's waiting callbacks are not pending work: they run only
// once the panel shows, so they must not keep the host from counting as idle.
bool hasPendingAnimationFrames() {
    if (!g_host) return false;
    for (const RafEntry& entry : g_host->rafPending) {
        if (!g_host->engine || !g_host->engine->isSystemDocumentHidden(entry.doc)) return true;
    }
    return false;
}

}  // namespace bro::bronze_host

// brokit in the bronze realm bro owns: everything the web platform provides
// that is not bound to the engine's clock, window or document.
//
// Two halves, one install point. The Node half — `require()` and the modules
// it hands back (`fs`, `path`, `os`, `child_process`, `net`, `dgram`), plus
// `process` — which bro apps have always had: the launcher and the project
// manager read the workshop with `fs` and spawn tools with `child_process`.
// And the web half — `fetch` with `Headers`/`Request`/`Response`, `URL`,
// `Blob`/`File`/`FileReader`, `TextEncoder`/`TextDecoder`, `btoa`/`atob`,
// `AbortController`, `WebSocket`, `EventSource`, `FormData`, `structuredClone`,
// the streams, `crypto`, `indexedDB`, `TreeWalker`, `EventTarget`,
// `MessageChannel` — which brokit already ships as bronze-compiled JS and C++
// (../brokit/src/api), and which this layer used to duplicate by hand.
//
// What is NOT installed from brokit, and why: `console` aside, every brokit
// installer that would REPLACE a value this layer binds to the engine is
// skipped — timers (`setTimeout` runs on the engine clock, virtual under
// headless advanceTime; host_timers.cpp), `navigator` (gamepads and the
// clipboard live on it; dom_gamepad.cpp), and `localStorage`/`sessionStorage`
// (per-document, persisted beside the app; dom_storage.cpp). A second
// registration of those names would swap the bound value for one that is not.
//
// Order follows brokit's own installAll() (api.cpp): the module registry
// first, because every module below registers into it; `EventTarget` before
// `MessageChannel`, whose `MessagePort` chains its prototype onto it at
// install time; `Buffer` after encoding and base64, which buffer.js reads at
// install time; `require` last.
//
// Three names are lifted from `globalThis` into bronze's host registry after
// the install that put them there. `bronze_global_get` asks the registry
// BEFORE the global object (bronze runtime/rt_global.cpp), and brokit's fetch
// is registered twice: fetch.cpp puts the native in the registry, then
// fetch_classes.js assigns the wrapper that understands `Request`, `Headers`
// and `FormData` bodies onto `globalThis.fetch`. Without the lift, compiled
// app code would read the native and lose that wrapper. `Buffer` and
// `structuredClone` are assigned by JS only and would resolve either way; they
// are lifted so all three read through the same path.
//
// Pumps: brokit's fetch, WebSocket, raw-socket and fs.watch completions are
// polled, not pushed — each family exposes a `__brokit_*_tick` the host calls
// once per turn. hostFrame (dom_globals.cpp) calls pumpBrokitTicks() as a
// host-task step, and headless advanceTime/flush call it so a `fetch().then`
// resolves across one advanceTime in a test.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_html_interfaces.h"

#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/object_url.h"

#include "api/api.h"

#include <memory>

namespace bro::bronze_host {

namespace {

// The `__brokit_*_tick` functions, looked up once after the install and held
// for the life of the process (the realm is process-lived; see the lifetime
// note at the top of dom_globals.cpp).
struct BrokitPumps {
    ev::Persistent fetchTick;
    ev::Persistent wsTick;
    ev::Persistent netTick;
    ev::Persistent fsWatchTick;
    ev::Persistent fetchHasPending;
    ev::Persistent wsHasPending;
    ev::Persistent netHasPending;
    ev::Persistent fsWatchHasPending;
};

BrokitPumps* g_pumps = nullptr;

Value globalProperty(const char* name) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return ev::undefined();
    return ev::getProperty(gt.value, name);
}

// `URL.createObjectURL(x)` for an x that is not a Blob is a TypeError on the
// web; brokit's url_object.js registers whatever it is handed and mints a URL
// that every later resolve then fails on. The registry stays brokit's — this
// wraps the function it installed with the type check in front, deciding
// Blob-ness the way brokit's own fetch does (blobBytes answers false for
// anything that is not a Blob or File).
void guardCreateObjectURL() {
    ev::GlobalValue url = ev::globalValue("URL");
    if (!url.found || !ev::isFunction(url.value)) return;
    ev::Persistent urlRoot(url.value);
    Value original = ev::getProperty(urlRoot.get(), "createObjectURL");
    if (!ev::isFunction(original)) return;
    auto held = std::make_shared<ev::Persistent>(original);
    Value guarded = ev::makeFunction(
        [held](Value thisValue, std::span<const Value> a) -> Value {
            const uint8_t* data = nullptr;
            size_t len = 0;
            if (a.empty() || !brokit::api::blobBytes(a[0], &data, &len)) {
                return ev::throwTypeError(
                    "URL.createObjectURL: the argument must be a Blob or a File");
            }
            ev::CallResult r = ev::call(held->get(), thisValue, a);
            if (r.thrown) return ev::throwValue(r.value);
            return r.value;
        },
        1, "createObjectURL");
    ev::setProperty(urlRoot.get(), "createObjectURL", guarded);
}

void callTick(const ev::Persistent& slot, const char* name) {
    Value fn = slot.get();
    if (!ev::isFunction(fn)) return;
    ev::CallResult r = ev::call(fn, ev::undefined(), {});
    if (r.thrown) reportBronzeError(name, r.value);
}

bool askPending(const ev::Persistent& slot) {
    Value fn = slot.get();
    if (!ev::isFunction(fn)) return false;
    ev::CallResult r = ev::call(fn, ev::undefined(), {});
    return !r.thrown && ev::toBool(r.value);
}

}  // namespace

void installBrokitGlobals(engine::Engine& engine) {
    namespace bk = brokit::api;

    bk::setHostTaskPoster(postHostTask);

    util::setObjectURLResolver([](const std::string& url) -> std::shared_ptr<const util::ObjectURLData> {
        bronze::Value blob = ev::undefined();
        if (!bk::blobByObjectURL(url, &blob)) return nullptr;
        const uint8_t* data = nullptr;
        size_t len = 0;
        std::string type;
        bk::blobBytes(blob, &data, &len, &type);
        auto out = std::make_shared<util::ObjectURLData>();
        if (data && len > 0) {
            out->bytes.assign(data, data + len);
        }
        out->type = std::move(type);
        return out;
    });

    bk::installModuleRegistry();
    bk::installConsole();
    bk::installURL();
    bk::installCrypto();
    bk::installSubtleCrypto();
    bk::installEncoding();
    bk::installTreeWalker();
    bk::installAbortController();
    bk::installStructuredClone();
    bk::installBlob();
    bk::installURLObject();
    guardCreateObjectURL();
    bk::installProcess();
    bk::installOS();
    bk::installPath();
    bk::installIndexedDB();
    bk::installIndexedDBJS();
    bk::installReadableStream();
    bk::installFetch();
    bk::installWritableStream();
    bk::installFS();
    bk::installFSWatch();
    bk::installChildProcess();
    bk::installWebSocket();
    bk::installWebSocketJS();
    bk::installEventSource();
    bk::installFormData();
    bk::installFetchClasses();
    adoptGlobalProperty("fetch");
    bk::installXMLHttpRequest();
    bk::installCompression();
    bk::installBase64();
    adoptGlobalProperty("btoa");
    adoptGlobalProperty("atob");
    bk::installEventTarget();
    bk::installMessageChannel();
    bk::installEvents();
    {
        ev::GlobalValue evt = ev::globalValue("Event");
        if (evt.found && ev::isFunction(evt.value)) {
            Value evtProto = ev::getProperty(evt.value, "prototype");
            if (ev::isObject(evtProto)) {
                ev::setPrototype(gamepadEventHostClass().prototype(), evtProto);
            }
        }
    }
    bk::installUtil();
    bk::installBuffer();
    adoptGlobalProperty("Buffer");
    adoptGlobalProperty("structuredClone");
    bk::installNet();
    bk::installNetJS();
    bk::installWebSocketServerJS();
    bk::installRequire();
    // Not here: bk::installImage(). It mounts onto whatever `bro` is
    // registered, and at this point none is — installBroRoots runs after
    // this and would replace the object it made. installBrokitImageKernels
    // below is called from there instead.

    // Where bro's virtual paths (`/app`, `/lib`, `/system`, ...) point, so
    // `fs.readFileSync('/app/data.json')` and `fetch('/app/data.json')` read
    // the file the engine's own loaders read (one mount table serves both),
    // and the app directory as the base a relative `fs` path or `fetch` URL
    // resolves against. IndexedDB files land beside the app, where
    // dom_storage.cpp keeps localStorage's `.storage.json`.
    for (const auto& [prefix, target] : engine.assetMounts().mounts()) {
        bk::addFsPrefixMount(prefix, target);
    }
    if (!engine.appDir().empty()) {
        bk::addFsBasePath(engine.appDir());
        bk::addFetchBasePath(engine.appDir());
        bk::setIndexedDBPath(engine.appDir());
    }

    // `document.createTreeWalker`: brokit's treewalker.js hands out an
    // installer that puts the method on whatever object it is given. Putting
    // it on Document's prototype rather than the `document` instance is what
    // makes a DOMParser result (a second Document; host_parser.cpp) answer
    // too.
    {
        Value hook = globalProperty("__brokit_install_createTreeWalker");
        if (ev::isFunction(hook)) {
            Value proto = documentHostClass().prototype();
            ev::CallResult r = ev::call(hook, ev::undefined(), std::span<const Value>(&proto, 1));
            if (r.thrown) reportBronzeError("brokit createTreeWalker install", r.value);
        }
    }

    // Never freed — see the lifetime note at the top of dom_globals.cpp.
    g_pumps = new BrokitPumps();
    g_pumps->fetchTick.set(globalProperty("__brokit_fetch_tick"));
    g_pumps->wsTick.set(globalProperty("__brokit_ws_tick"));
    g_pumps->netTick.set(globalProperty("__brokit_net_tick"));
    g_pumps->fsWatchTick.set(globalProperty("__brokit_fs_watch_tick"));
    g_pumps->fetchHasPending.set(globalProperty("__brokit_fetch_has_pending"));
    g_pumps->wsHasPending.set(globalProperty("__brokit_ws_has_pending"));
    g_pumps->netHasPending.set(globalProperty("__brokit_net_has_pending"));
    g_pumps->fsWatchHasPending.set(globalProperty("__brokit_fs_watch_has_pending"));
}

// The bro.image typed-array kernels (reduce/map/combine/lookup/stencil/
// resample, gradient, alloc — brokit's src/api/image.cpp over broimage).
// brokit's installer finds the registered `bro` and sets `bro.image` on it,
// so it runs from installBroRoots once that root exists; the codec and gpu
// members are then added to the object it made (host_bro_root.cpp).
void installBrokitImageKernels() {
    brokit::api::installImage();
}

void pumpBrokitTicks() {
    if (!g_pumps) return;
    callTick(g_pumps->fetchTick, "__brokit_fetch_tick");
    callTick(g_pumps->wsTick, "__brokit_ws_tick");
    callTick(g_pumps->netTick, "__brokit_net_tick");
    callTick(g_pumps->fsWatchTick, "__brokit_fs_watch_tick");
}

bool brokitHasPendingWork() {
    if (!g_pumps) return false;
    return askPending(g_pumps->fetchHasPending) ||
           askPending(g_pumps->wsHasPending) ||
           askPending(g_pumps->netHasPending) ||
           askPending(g_pumps->fsWatchHasPending);
}

}  // namespace bro::bronze_host

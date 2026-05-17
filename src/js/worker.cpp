#include "js/worker.h"
#include "util/asset_mounts.h"
#include "js/ai_bindings.h"
#include "js/mesh_bindings.h"
#include "js/flora_bindings.h"
#include "js/math_bindings.h"
#include "js/message_serializer.h"
#include "js/net_bindings.h"
#include "js/runtime.h"
#include "js/server_bindings.h"
#include "js/timers.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <api/api.h>  // brokit::api

#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_map>

extern "C" {
#include "quickjs.h"
}

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ============================================================================
// Worker implementation
// ============================================================================

Worker::Worker(const std::string& scriptPath, const std::string& basePath,
               net::NetService* netService,
               const util::AssetMounts* mounts)
    : scriptPath_(scriptPath)
    , basePath_(basePath)
    , netService_(netService)
    , mounts_(mounts)
{
}

Worker::~Worker()
{
    terminate();
}

void Worker::setTickRate(double hz)
{
    if (hz < 1.0) hz = 1.0;
    if (hz > 1000.0) hz = 1000.0;
    tickRate_.store(hz, std::memory_order_relaxed);
}

double Worker::uptimeSec() const
{
    double start = startTimeMs_.load(std::memory_order_relaxed);
    if (start <= 0.0) return 0.0;
    return (util::currentTimeMs() - start) / 1000.0;
}

void Worker::start()
{
    alive_.store(true, std::memory_order_release);
    thread_ = std::thread(&Worker::threadFunc, this);
}

void Worker::terminate()
{
    terminated_.store(true, std::memory_order_release);
    wakeup_.fetch_add(1, std::memory_order_release);
    wakeup_.notify_one();

    // threadFunc clears alive_ before returning, so alive_ can be false while
    // the thread handle is still joinable — always join based on the handle.
    if (thread_.joinable())
        thread_.join();

    // Drain any remaining messages
    toWorker_.clear();
    fromWorker_.clear();
}

bool Worker::postMessage(JSContext* mainCtx, JSValue value, JSValue transferList)
{
    auto* msg = new Message();
    if (!serializeMessage(mainCtx, value, transferList, *msg)) {
        delete msg;
        return false;
    }
    if (!toWorker_.push(msg)) {
        delete msg;
        JS_ThrowTypeError(mainCtx, "Worker message queue full");
        return false;
    }
    // Wake the worker thread
    wakeup_.fetch_add(1, std::memory_order_release);
    wakeup_.notify_one();
    return true;
}

void Worker::drainMessages(JSContext* mainCtx)
{
    while (Message* msg = fromWorker_.pop()) {
        // Deserialize on main context
        JSValue data = deserializeMessage(mainCtx, *msg);
        delete msg;

        if (JS_IsException(data)) {
            Runtime::checkException(mainCtx, data);
            continue;
        }

        // Call worker.onmessage({data: ...}) on main thread
        JSValue onmsg = JS_GetPropertyStr(mainCtx, jsObject, "onmessage");
        if (JS_IsFunction(mainCtx, onmsg)) {
            JSValue event = JS_NewObject(mainCtx);
            JS_SetPropertyStr(mainCtx, event, "data", data);  // takes ownership of data

            JSValue ret = JS_Call(mainCtx, onmsg, jsObject, 1, &event);
            if (JS_IsException(ret))
                Runtime::checkException(mainCtx, ret);
            else
                JS_FreeValue(mainCtx, ret);
            JS_FreeValue(mainCtx, event);
        } else {
            JS_FreeValue(mainCtx, data);
        }
        JS_FreeValue(mainCtx, onmsg);
    }
}

// ---------------------------------------------------------------------------
// Worker thread — self-contained event loop
// ---------------------------------------------------------------------------

/// Per-worker-thread handle. Stored in a thread_local (not the JSContext
/// opaque slot — that belongs to Timers, and each worker thread only ever
/// runs one JSContext, so a thread-local is unambiguous).
struct WorkerCtxData {
    Worker* worker = nullptr;
    Timers* timers = nullptr;
};
static thread_local WorkerCtxData* s_wcd = nullptr;

static JSValue js_worker_self_postMessage(JSContext* ctx, JSValueConst /*this_val*/,
                                          int argc, JSValueConst* argv)
{
    auto* wcd = s_wcd;
    if (!wcd || !wcd->worker) return JS_UNDEFINED;

    JSValue value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue transferList = argc > 1 ? argv[1] : JS_UNDEFINED;

    auto* msg = new Message();
    if (!serializeMessage(ctx, value, transferList, *msg)) {
        delete msg;
        return JS_EXCEPTION;
    }
    if (!wcd->worker->pushToMain(msg)) {
        delete msg;
        return JS_ThrowTypeError(ctx, "Worker message queue full");
    }
    return JS_UNDEFINED;
}

static JSValue js_worker_self_close(JSContext* /*ctx*/, JSValueConst /*this_val*/,
                                    int /*argc*/, JSValueConst* /*argv*/)
{
    if (s_wcd && s_wcd->worker)
        s_wcd->worker->requestClose();
    return JS_UNDEFINED;
}

void Worker::threadFunc()
{
    // --- 1. Create isolated JSRuntime + JSContext ---
    auto runtime = std::make_unique<Runtime>();
    runtime->setModuleLoader();
    JSContext* ctx = runtime->getContext();

    // Per-worker JS interrupt handler. terminate() flips terminated_ and
    // joins the thread, but if the worker is currently executing JS (most
    // notably during synchronous top-level script load — e.g. the
    // stompworld trainer's BC warmup runs before the event loop is even
    // entered), the join would block forever otherwise. The handler is
    // polled by QuickJS every N bytecodes, returning non-zero aborts the
    // current JS_Call with an uncatchable interrupt exception. Honors the
    // process-wide Ctrl+C flag too so workers shut down on signals.
    JS_SetInterruptHandler(runtime->getRuntime(),
        [](JSRuntime* /*rt*/, void* opaque) -> int {
            auto* self = static_cast<Worker*>(opaque);
            if (self->terminated_.load(std::memory_order_relaxed)) return 1;
            if (bro::util::interrupted()) return 1;
            return 0;
        }, this);

    // --- 2. Install brokit APIs (no DOM, no canvas, no window) ---
    // Use installAll to get console, timers, encoding, URL, crypto, base64,
    // structuredClone, fetch, streams, noise, etc.
    brokit::api::installAll(ctx);
    brokit::api::addFetchBasePath(ctx, basePath_);
    brokit::api::addFsBasePath(ctx, basePath_);

    // Inherit engine prefix mounts (/lib, /system, ...) from the parent
    // bindings state so a worker can `require('/lib/foo.js')` etc.
    if (mounts_) {
        for (const auto& [prefix, target] : mounts_->mounts()) {
            brokit::api::addFsPrefixMount(ctx, prefix, target);
            brokit::api::addFetchPrefixMount(ctx, prefix, target);
        }
    }

    // --- 3. Install own Timers ---
    auto timers = std::make_unique<Timers>();
    Timers::install(ctx, timers.get());

    // --- 3b. Install Mesh class (for marching cubes / mesh generation in workers) ---
    MeshBindings::install(ctx);

    // --- 3b'. Install bro.math.* (SpatialHash3D, plus future bromath types) ---
    MathBindings::install(ctx);

    // --- 3b''. Install bro.flora.* (broflora ecosystem sim) ---
    FloraBindings::install(ctx);

    // --- 3b'. Install bro.ai.game (navmesh, pathfinding, LOS, steering).
    // No engine dependency; all state lives on JS-owned wrapper objects. ---
    AIBindings::install(ctx);

    // --- 3b''. Install bro.tensor (GPU tensor + ops, brotensor sibling). ---
    installTensorBindings(ctx);

    // --- 3c. Install bro.net bindings (own subscriber against shared
    // NetService). The service is thread-safe by design — commands/events
    // are routed through lock-free per-subscriber queues. ---
    if (netService_) {
        NetBindings::install(ctx, netService_);
    }

    // --- 3d. Install bro.server bindings scoped to this worker. Exposes
    // tickrate (rate-limits our event loop), uptime (seconds since this
    // thread started), and stop() (terminates this worker). ---
    startTimeMs_.store(util::currentTimeMs(), std::memory_order_relaxed);
    ServerBindings::installWorker(ctx, this);

    // --- 4. Store context data for C callbacks via thread-local. The
    // JSContext opaque slot belongs to Timers on this context and must
    // not be overwritten. ---
    WorkerCtxData wcd;
    wcd.worker = this;
    wcd.timers = timers.get();
    s_wcd = &wcd;

    // --- 5. Install worker globals: self, postMessage, close, onmessage ---
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "postMessage",
        JS_NewCFunction(ctx, js_worker_self_postMessage, "postMessage", 1));
    JS_SetPropertyStr(ctx, global, "close",
        JS_NewCFunction(ctx, js_worker_self_close, "close", 0));
    // onmessage defaults to undefined — user sets it in worker script
    JS_FreeValue(ctx, global);

    // --- 6. Resolve and load worker script ---
    std::string fullPath = scriptPath_;
    if (!std::filesystem::path(fullPath).is_absolute())
        fullPath = basePath_ + "/" + scriptPath_;

    bool loadOk = runtime->loadFile(fullPath);
    if (!loadOk
        && !terminated_.load(std::memory_order_relaxed)
        && !bro::util::interrupted())
    {
        LOG_ERROR("Worker: failed to load script '%s'", fullPath.c_str());
    }
    runtime->executePendingJobs();

    // --- 7. Event loop ---
    // Skipped if the script failed to load OR was interrupted mid-load
    // (terminated_ already set). Cleanup at the bottom still runs in both
    // cases so bindings get torn down and the GC drains JS reference
    // cycles before the runtime destructor — otherwise the dtor can
    // assert on a non-empty gc_obj_list.
    while (loadOk && !terminated_.load(std::memory_order_acquire)) {
        // Process-wide Ctrl+C: interrupts any JS currently running via the
        // QuickJS interrupt handler; here we also exit the loop so workers
        // idling between messages shut down.
        if (bro::util::interrupted())
            break;

        double tickStart = util::currentTimeMs();
        double intervalMs = 1000.0 / tickRate_.load(std::memory_order_relaxed);

        // Process incoming messages (main → worker)
        while (Message* msg = toWorker_.pop()) {
            JSValue data = deserializeMessage(ctx, *msg);
            delete msg;

            if (JS_IsException(data)) {
                Runtime::checkException(ctx, data);
                continue;
            }

            // Call self.onmessage({data: ...})
            JSValue g = JS_GetGlobalObject(ctx);
            JSValue onmsg = JS_GetPropertyStr(ctx, g, "onmessage");
            if (JS_IsFunction(ctx, onmsg)) {
                JSValue event = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, event, "data", data);  // takes ownership

                JSValue ret = JS_Call(ctx, onmsg, g, 1, &event);
                if (JS_IsException(ret))
                    Runtime::checkException(ctx, ret);
                else
                    JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, event);
            } else {
                JS_FreeValue(ctx, data);
            }
            JS_FreeValue(ctx, onmsg);
            JS_FreeValue(ctx, g);
        }

        // Tick timers
        double now = util::currentTimeMs();
        timers->tick(now);

        // Drain microtasks
        runtime->executePendingJobs();

        // Tick fetch (pump pending HTTP requests)
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue tickFn = JS_GetPropertyStr(ctx, g, "__brokit_fetch_tick");
        if (JS_IsFunction(ctx, tickFn)) {
            JSValue ret = JS_Call(ctx, tickFn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, tickFn);
        JS_FreeValue(ctx, g);

        // Tick WebSocket (pump pending connections/messages)
        g = JS_GetGlobalObject(ctx);
        tickFn = JS_GetPropertyStr(ctx, g, "__brokit_ws_tick");
        if (JS_IsFunction(ctx, tickFn)) {
            JSValue ret = JS_Call(ctx, tickFn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, tickFn);
        JS_FreeValue(ctx, g);

        // Tick fs.watch (deliver native filesystem events to JS)
        g = JS_GetGlobalObject(ctx);
        tickFn = JS_GetPropertyStr(ctx, g, "__brokit_fs_watch_tick");
        if (JS_IsFunction(ctx, tickFn)) {
            JSValue ret = JS_Call(ctx, tickFn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, tickFn);
        JS_FreeValue(ctx, g);

        // Drain this subscriber's network events — fires JS onconnect /
        // ondisconnect / onmessage callbacks on this worker thread.
        if (netService_) {
            NetBindings::poll(ctx);
            runtime->executePendingJobs();
        }

        // Rate-limit to the configured tick rate. At the default 1000 Hz
        // this sleeps ~1ms when idle (matching legacy behavior); at 60 Hz
        // it yields ~16ms, dramatically reducing CPU for server workers.
        // When the iteration itself takes longer than the interval (heavy
        // compute, message burst), sleepMs <= 0 and the loop spins on —
        // same as bro-server's server-mode loop.
        double elapsed = util::currentTimeMs() - tickStart;
        double sleepMs = intervalMs - elapsed;
        if (sleepMs > 0.5) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<int64_t>(sleepMs * 1000.0)));
        }
    }

    // --- 8. Cleanup ---
    timers->clearAll(ctx);
    ServerBindings::cleanup(ctx);
    if (netService_) {
        NetBindings::cleanup(ctx);
    }
    AIBindings::cleanup(ctx);
    MeshBindings::cleanup(ctx);
    s_wcd = nullptr;
    JS_SetContextOpaque(ctx, nullptr);

    // Break the worker-specific reference cycles before the runtime
    // destructor's gc_obj_list assertion fires. `self` is the global
    // pointing at itself (self-loop refcount); `onmessage` is the user's
    // JS closure which captures script-scope locals (e.g. agent / sim /
    // tilemap → C++-bound qjsbind classes), keeping their finalizers
    // unreachable. Deleting both lets the global drop its strong refs to
    // them so the cascade reaches everything reachable only through
    // those handles. Plain GC alone can't break the self-loop because
    // the global is also the GC root.
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom aSelf      = JS_NewAtom(ctx, "self");
        JSAtom aOnmessage = JS_NewAtom(ctx, "onmessage");
        JS_DeleteProperty(ctx, global, aSelf, 0);
        JS_DeleteProperty(ctx, global, aOnmessage, 0);
        JS_FreeAtom(ctx, aSelf);
        JS_FreeAtom(ctx, aOnmessage);
        JS_FreeValue(ctx, global);
    }

    // Drain microtasks and run GC to break reference cycles (e.g. the
    // Mesh prototype↔constructor cycle from JS_SetConstructor) before
    // the runtime destructor asserts on a non-empty gc_obj_list.
    // qjsbind classes that DupValue JS callbacks must register a gc_mark
    // (e.g. AIGridObsWindow, AIGenericMcts) so the cycle GC can see those
    // refs — without it, refcount-only release can't break wrapper-↔-
    // closure cycles and the runtime asserts on shutdown.
    runtime->executePendingJobs();
    JS_RunGC(runtime->getRuntime());
    runtime->executePendingJobs();
    JS_RunGC(runtime->getRuntime());
    // runtime dtor frees JSContext + JSRuntime

    alive_.store(false, std::memory_order_release);
}

// ============================================================================
// JS bindings — Worker constructor on main thread
// ============================================================================

struct WorkerOpaque {
    Worker* worker = nullptr;
};

// Per-context state: base path for resolving worker scripts, and the
// NetService pointer that spawned workers should bind to.
struct WorkerBindingsState {
    std::string basePath;
    std::vector<Worker*> workers;  // all workers created from this context
    net::NetService* netService = nullptr;
    const util::AssetMounts* mounts = nullptr;
};

static std::unordered_map<JSContext*, WorkerBindingsState> s_workerState;

static void js_worker_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* opaque = static_cast<WorkerOpaque*>(
        JS_GetOpaque(val, qjsbind::class_id<WorkerOpaque>()));
    if (opaque) {
        if (opaque->worker)
            opaque->worker->terminate();
        delete opaque;
    }
}

static JSValue js_worker_ctor(JSContext* ctx, JSValueConst new_target,
                              int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Worker constructor requires a script path string");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    auto it = s_workerState.find(ctx);
    if (it == s_workerState.end()) {
        JS_FreeCString(ctx, path);
        return JS_ThrowInternalError(ctx, "Worker bindings not initialized");
    }

    std::string scriptPath(path);
    JS_FreeCString(ctx, path);

    // Engine-supplied mounts (e.g. /lib/foo.js) take precedence over basePath.
    if (it->second.mounts) {
        std::string resolved = it->second.mounts->resolve(scriptPath);
        if (!resolved.empty()) scriptPath = resolved;
    }

    // Create C++ Worker
    auto* worker = new Worker(scriptPath, it->second.basePath,
                              it->second.netService, it->second.mounts);
    it->second.workers.push_back(worker);

    // Create JS object
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, qjsbind::class_id<WorkerOpaque>());
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete worker;
        return obj;
    }

    auto* opaque = new WorkerOpaque();
    opaque->worker = worker;
    JS_SetOpaque(obj, opaque);

    // Store JS object reference on the worker for drainMessages
    worker->jsObject = JS_DupValue(ctx, obj);

    // Start the worker thread
    worker->start();

    return obj;
}

static JSValue js_worker_postMessage(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv)
{
    auto* opaque = qjsbind::unwrap<WorkerOpaque>(ctx, this_val);
    if (!opaque || !opaque->worker || !opaque->worker->isAlive())
        return JS_ThrowTypeError(ctx, "Worker is not running");

    JSValue value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue transferList = argc > 1 ? argv[1] : JS_UNDEFINED;

    if (!opaque->worker->postMessage(ctx, value, transferList))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_worker_terminate(JSContext* ctx, JSValueConst this_val,
                                   int /*argc*/, JSValueConst* /*argv*/)
{
    auto* opaque = qjsbind::unwrap<WorkerOpaque>(ctx, this_val);
    if (opaque && opaque->worker)
        opaque->worker->terminate();
    return JS_UNDEFINED;
}

void installWorkerBindings(JSContext* ctx, const std::string& appBasePath,
                           net::NetService* netService,
                           const util::AssetMounts* mounts)
{
    // Register class and prototype via qjsbind (NoGlobal — we set a custom constructor below)
    qjsbind::Class<WorkerOpaque>(ctx, "Worker", qjsbind::NoGlobal,
                                  js_worker_finalizer)
        .method_raw("postMessage", js_worker_postMessage, 1)
        .method_raw("terminate", js_worker_terminate, 0);

    // Install custom constructor on globalThis (needs new_target access)
    JSValue proto = JS_GetClassProto(ctx, qjsbind::class_id<WorkerOpaque>());
    JSValue ctor = JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 1,
                                    JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Worker", ctor);
    JS_FreeValue(ctx, global);

    // Per-context state
    s_workerState[ctx] = WorkerBindingsState{appBasePath, {}, netService, mounts};
}

void cleanupWorkerBindings(JSContext* ctx)
{
    auto it = s_workerState.find(ctx);
    if (it == s_workerState.end()) return;

    // Terminate and free all workers
    for (Worker* w : it->second.workers) {
        w->terminate();
        if (!JS_IsUndefined(w->jsObject)) {
            // The JS Worker object may still be referenced elsewhere (e.g.
            // in a cycle broken later by JS_RunGC during JS_FreeRuntime).
            // Null out the opaque's back-pointer so its finalizer won't
            // dereference the Worker we're about to delete.
            auto* opaque = qjsbind::unwrap<WorkerOpaque>(ctx, w->jsObject);
            if (opaque) opaque->worker = nullptr;
            JS_FreeValue(ctx, w->jsObject);
            w->jsObject = JS_UNDEFINED;
        }
        delete w;
    }
    s_workerState.erase(it);
}

// Called by Engine once per frame to drain messages from all workers.
void tickWorkers(JSContext* ctx)
{
    auto it = s_workerState.find(ctx);
    if (it == s_workerState.end()) return;

    for (Worker* w : it->second.workers) {
        if (w->isAlive())
            w->drainMessages(ctx);
    }
}

}  // namespace bro::js

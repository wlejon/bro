#include "js/worker.h"
#include "js/mesh_bindings.h"
#include "js/message_serializer.h"
#include "js/runtime.h"
#include "js/timers.h"
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

namespace bro::js {

// ============================================================================
// Worker implementation
// ============================================================================

Worker::Worker(const std::string& scriptPath, const std::string& basePath)
    : scriptPath_(scriptPath)
    , basePath_(basePath)
{
}

Worker::~Worker()
{
    terminate();
}

void Worker::start()
{
    alive_.store(true, std::memory_order_release);
    thread_ = std::thread(&Worker::threadFunc, this);
}

void Worker::terminate()
{
    if (!alive_.load(std::memory_order_acquire))
        return;

    terminated_.store(true, std::memory_order_release);
    wakeup_.fetch_add(1, std::memory_order_release);
    wakeup_.notify_one();

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

/// Stored in the worker JSContext's opaque to access the Worker from C callbacks.
struct WorkerCtxData {
    Worker* worker = nullptr;
    Timers* timers = nullptr;
};

static JSValue js_worker_self_postMessage(JSContext* ctx, JSValueConst /*this_val*/,
                                          int argc, JSValueConst* argv)
{
    auto* wcd = static_cast<WorkerCtxData*>(JS_GetContextOpaque(ctx));
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

static JSValue js_worker_self_close(JSContext* ctx, JSValueConst /*this_val*/,
                                    int /*argc*/, JSValueConst* /*argv*/)
{
    auto* wcd = static_cast<WorkerCtxData*>(JS_GetContextOpaque(ctx));
    if (wcd && wcd->worker)
        wcd->worker->requestClose();
    return JS_UNDEFINED;
}

void Worker::threadFunc()
{
    // --- 1. Create isolated JSRuntime + JSContext ---
    auto runtime = std::make_unique<Runtime>();
    runtime->setModuleLoader();
    JSContext* ctx = runtime->getContext();

    // --- 2. Install brokit APIs (no DOM, no canvas, no window) ---
    // Use installAll to get console, timers, encoding, URL, crypto, base64,
    // structuredClone, fetch, streams, noise, etc.
    brokit::api::installAll(ctx);
    brokit::api::addFetchBasePath(ctx, basePath_);
    brokit::api::addFsBasePath(ctx, basePath_);

    // --- 3. Install own Timers ---
    auto timers = std::make_unique<Timers>();
    Timers::install(ctx, timers.get());

    // --- 3b. Install Mesh class (for marching cubes / mesh generation in workers) ---
    MeshBindings::install(ctx);

    // --- 4. Store context data for C callbacks ---
    WorkerCtxData wcd;
    wcd.worker = this;
    wcd.timers = timers.get();
    JS_SetContextOpaque(ctx, &wcd);

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

    if (!runtime->loadFile(fullPath)) {
        LOG_ERROR("Worker: failed to load script '%s'", fullPath.c_str());
        alive_.store(false, std::memory_order_release);
        return;
    }
    runtime->executePendingJobs();

    // --- 7. Event loop ---
    while (!terminated_.load(std::memory_order_acquire)) {
        bool didWork = false;

        // Process incoming messages (main → worker)
        while (Message* msg = toWorker_.pop()) {
            JSValue data = deserializeMessage(ctx, *msg);
            delete msg;
            didWork = true;

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

        // If no immediate work, sleep briefly to avoid busy-spinning.
        // 1ms gives good timer precision without burning CPU.
        if (!didWork) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // --- 8. Cleanup ---
    timers->clearAll(ctx);
    MeshBindings::cleanup(ctx);
    JS_SetContextOpaque(ctx, nullptr);

    // Drain microtasks and run GC to break reference cycles (e.g. the
    // Mesh prototype↔constructor cycle from JS_SetConstructor) before
    // the runtime destructor asserts on a non-empty gc_obj_list.
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

static JSClassID js_worker_class_id = 0;

struct WorkerOpaque {
    Worker* worker = nullptr;
};

// Per-context state: base path for resolving worker scripts
struct WorkerBindingsState {
    std::string basePath;
    std::vector<Worker*> workers;  // all workers created from this context
};

static std::unordered_map<JSContext*, WorkerBindingsState> s_workerState;

static void js_worker_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* opaque = static_cast<WorkerOpaque*>(JS_GetOpaque(val, js_worker_class_id));
    if (opaque) {
        if (opaque->worker)
            opaque->worker->terminate();
        delete opaque;
    }
}

static JSClassDef js_worker_class = {
    .class_name = "Worker",
    .finalizer = js_worker_finalizer,
};

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

    // Create C++ Worker
    auto* worker = new Worker(scriptPath, it->second.basePath);
    it->second.workers.push_back(worker);

    // Create JS object
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_worker_class_id);
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
    auto* opaque = static_cast<WorkerOpaque*>(JS_GetOpaque(this_val, js_worker_class_id));
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
    auto* opaque = static_cast<WorkerOpaque*>(JS_GetOpaque(this_val, js_worker_class_id));
    if (opaque && opaque->worker)
        opaque->worker->terminate();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_worker_proto_funcs[] = {
    JS_CFUNC_DEF("postMessage", 1, js_worker_postMessage),
    JS_CFUNC_DEF("terminate", 0, js_worker_terminate),
};

void installWorkerBindings(JSContext* ctx, const std::string& appBasePath)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Register class (once per runtime)
    if (js_worker_class_id == 0)
        JS_NewClassID(rt, &js_worker_class_id);
    JS_NewClass(rt, js_worker_class_id, &js_worker_class);

    // Prototype
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_worker_proto_funcs,
                               sizeof(js_worker_proto_funcs) / sizeof(js_worker_proto_funcs[0]));
    JS_SetClassProto(ctx, js_worker_class_id, proto);

    // Constructor
    JSValue ctor = JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 1,
                                    JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);

    // Install on global
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Worker", ctor);
    JS_FreeValue(ctx, global);

    // Per-context state
    s_workerState[ctx] = WorkerBindingsState{appBasePath, {}};
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
            auto* opaque = static_cast<WorkerOpaque*>(
                JS_GetOpaque(w->jsObject, js_worker_class_id));
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

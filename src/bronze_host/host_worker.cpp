#include "bronze_host/host_worker_msg.h"
#include "bronze_host/app_module.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_rejection_events.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include "eval/eval.h"
#include <api/api.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

// The event objects handed to onerror / onmessage and the listeners. Built
// through ObjectBuilder, whose set() reads the receiver only after the value
// argument (a fromUtf8 that allocates) has been made — the order a direct
// setProperty(obj.get(), key, fromUtf8(...)) does not guarantee.
Value makeErrorEvent(const std::string& message, const std::string& filename, int lineno) {
    ObjectBuilder evt;
    evt.set("type", ev::fromUtf8("error"));
    evt.set("message", ev::fromUtf8(message));
    evt.set("filename", ev::fromUtf8(filename));
    evt.set("lineno", ev::fromDouble(lineno));
    return evt.get();
}

Value makeMessageEvent(const ev::Persistent& data) {
    ObjectBuilder evt;
    evt.set("type", ev::fromUtf8("message"));
    evt.set("data", data.get());
    return evt.get();
}

class WorkerInstance;
static std::mutex s_workersMutex;
static std::vector<WorkerInstance*> s_activeWorkers;
static std::atomic<uint64_t> s_workerIdSeq{1};

class WorkerInstance {
public:
    WorkerInstance(std::string scriptPath, std::string basePath, const util::AssetMounts* mounts)
        : scriptPath_(std::move(scriptPath)), basePath_(std::move(basePath)), mounts_(mounts)
    {
        workerId_ = s_workerIdSeq.fetch_add(1, std::memory_order_relaxed);
    }

    ~WorkerInstance() {
        terminate();
    }

    void start() {
        alive_.store(true, std::memory_order_release);
        workerThread_ = std::thread(&WorkerInstance::threadFunc, this);
    }

    void terminate() {
        terminated_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(toWorkerMutex_);
            toWorkerCv_.notify_all();
        }
        if (workerThread_.joinable()) {
            if (std::this_thread::get_id() != workerThread_.get_id()) {
                workerThread_.join();
            }
        }
        alive_.store(false, std::memory_order_release);
    }

    void postToWorker(std::unique_ptr<Message> msg) {
        if (terminated_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(toWorkerMutex_);
        toWorkerQueue_.push_back(std::move(msg));
        toWorkerCv_.notify_one();
    }

    void postToMain(std::unique_ptr<Message> msg) {
        std::lock_guard<std::mutex> lock(toMainMutex_);
        toMainQueue_.push_back(std::move(msg));
    }

    struct Listener {
        std::string type;
        ev::Persistent fn;
    };

    void drainMessagesToMain() {
        std::deque<std::unique_ptr<Message>> batch;
        {
            std::lock_guard<std::mutex> lock(toMainMutex_);
            batch.swap(toMainQueue_);
        }
        if (batch.empty()) return;

        for (auto& msg : batch) {
            if (msg->isError) {
                ev::Persistent evt(makeErrorEvent(msg->errorMessage, msg->errorFilename,
                                                  msg->errorLineno));

                if (ev::isFunction(onerror_.get())) {
                    ev::Persistent cbRoot(onerror_.get());
                    Value event = evt.get();
                    ev::call(cbRoot.get(), ev::undefined(), std::span<const Value>(&event, 1));
                }

                std::vector<ev::Persistent> errorListeners;
                for (const auto& entry : listeners_) {
                    if (entry.type == "error" && ev::isFunction(entry.fn.get())) {
                        errorListeners.emplace_back(entry.fn.get());
                    }
                }
                for (auto& lfn : errorListeners) {
                    Value event = evt.get();
                    ev::call(lfn.get(), ev::undefined(), std::span<const Value>(&event, 1));
                }
            } else {
                ev::Persistent dataRoot(deserializeMessage(*msg));
                ev::Persistent evt(makeMessageEvent(dataRoot));

                if (ev::isFunction(onmessage_.get())) {
                    ev::Persistent cbRoot(onmessage_.get());
                    Value event = evt.get();
                    ev::call(cbRoot.get(), ev::undefined(), std::span<const Value>(&event, 1));
                }

                std::vector<ev::Persistent> msgListeners;
                for (const auto& entry : listeners_) {
                    if (entry.type == "message" && ev::isFunction(entry.fn.get())) {
                        msgListeners.emplace_back(entry.fn.get());
                    }
                }
                for (auto& lfn : msgListeners) {
                    Value event = evt.get();
                    ev::call(lfn.get(), ev::undefined(), std::span<const Value>(&event, 1));
                }
            }
        }
    }

    bool isAlive() const { return alive_.load(std::memory_order_acquire); }

    void setOnMessage(Value cb) { onmessage_.set(cb); }
    Value getOnMessage() const { return onmessage_.get(); }
    void setOnError(Value cb) { onerror_.set(cb); }
    Value getOnError() const { return onerror_.get(); }

    void addEventListener(const std::string& type, Value fn) {
        if (!ev::isFunction(fn)) return;
        ev::Persistent fnP(fn);
        for (const auto& entry : listeners_) {
            if (entry.type == type && ev::toBits(entry.fn.get()) == ev::toBits(fnP.get())) {
                return;
            }
        }
        listeners_.push_back({type, std::move(fnP)});
    }

    void removeEventListener(const std::string& type, Value fn) {
        if (!ev::isFunction(fn)) return;
        ev::Persistent fnP(fn);
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (it->type == type && ev::toBits(it->fn.get()) == ev::toBits(fnP.get())) {
                listeners_.erase(it);
                return;
            }
        }
    }

private:
    void threadFunc();

    std::string scriptPath_;
    std::string basePath_;
    const util::AssetMounts* mounts_ = nullptr;
    uint64_t workerId_ = 0;

    std::thread workerThread_;
    std::atomic<bool> alive_{false};
    std::atomic<bool> terminated_{false};

    std::mutex toWorkerMutex_;
    std::condition_variable toWorkerCv_;
    std::deque<std::unique_ptr<Message>> toWorkerQueue_;

    std::mutex toMainMutex_;
    std::deque<std::unique_ptr<Message>> toMainQueue_;

    ev::Persistent onmessage_;
    ev::Persistent onerror_;
    std::vector<Listener> listeners_;
};

void WorkerInstance::threadFunc() {
    ensureSharedRuntimeEnv();

    installImageBitmapGlobals();
    installNoiseGlobals();
    // The bro.math classes BEFORE the roots, as in the main realm: the root
    // aliases their constructors (host_math_funcs.cpp). Per thread, like
    // ImageBitmap's.
    installMathGlobals();

    // bro.server on this thread names THIS loop (native_server.cpp): stop()
    // ends it the way close() does, and the tick rate is the cadence the
    // idle wait below runs at. Set before the roots so the natives are wired
    // when the script reads them.
    const auto startedAt = std::chrono::steady_clock::now();
    std::atomic<double> tickRateHz{0.0};  // 0 = the default idle cadence
    WorkerServerControl serverControl;
    serverControl.tickRate = [&tickRateHz] {
        const double hz = tickRateHz.load(std::memory_order_relaxed);
        return hz > 0.0 ? hz : 60.0;
    };
    serverControl.setTickRate = [&tickRateHz](double hz) { tickRateHz.store(hz, std::memory_order_relaxed); };
    serverControl.uptimeSec = [startedAt] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
    };
    serverControl.stop = [this] { terminated_.store(true, std::memory_order_release); };
    setWorkerServerControl(&serverControl);

    ev::Persistent workerOnmessage;
    ev::Persistent workerOnerror;

    struct WorkerListener {
        std::string type;
        ev::Persistent fn;
    };
    std::vector<WorkerListener> workerListeners;

    auto addWorkerListener = [&workerListeners](const std::string& type, Value fn) {
        if (!ev::isFunction(fn)) return;
        ev::Persistent fnP(fn);
        for (const auto& entry : workerListeners) {
            if (entry.type == type && ev::toBits(entry.fn.get()) == ev::toBits(fnP.get())) {
                return;
            }
        }
        workerListeners.push_back({type, std::move(fnP)});
    };

    auto removeWorkerListener = [&workerListeners](const std::string& type, Value fn) {
        if (!ev::isFunction(fn)) return;
        ev::Persistent fnP(fn);
        for (auto it = workerListeners.begin(); it != workerListeners.end(); ++it) {
            if (it->type == type && ev::toBits(it->fn.get()) == ev::toBits(fnP.get())) {
                workerListeners.erase(it);
                return;
            }
        }
    };

    auto dispatchWorkerError = [&](const std::string& msg, const std::string& fn, int line) {
        auto errMsg = std::make_unique<Message>();
        errMsg->isError = true;
        errMsg->errorMessage = msg;
        errMsg->errorFilename = fn;
        errMsg->errorLineno = line;
        postToMain(std::move(errMsg));

        // The handler is rooted the moment it is read: building the event
        // below allocates, and a raw function Value held across that would be
        // called at its pre-collection address.
        ev::Persistent cb(workerOnerror.get());
        if (!ev::isFunction(cb.get())) {
            Value gt = ev::globalValue("globalThis").value;
            if (ev::isObject(gt)) {
                Value p = ev::getProperty(gt, "onerror");
                if (ev::isFunction(p)) cb.set(p);
            }
        }

        ev::Persistent errEvt(makeErrorEvent(msg, fn, line));

        if (ev::isFunction(cb.get())) {
            Value arg = errEvt.get();
            ev::call(cb.get(), ev::undefined(), std::span<const Value>(&arg, 1));
        }

        std::vector<ev::Persistent> errorListeners;
        for (const auto& l : workerListeners) {
            if (l.type == "error" && ev::isFunction(l.fn.get())) {
                errorListeners.emplace_back(l.fn.get());
            }
        }
        for (auto& lfn : errorListeners) {
            Value arg = errEvt.get();
            ev::call(lfn.get(), ev::undefined(), std::span<const Value>(&arg, 1));
        }
    };

    {
        // registerGlobal allocates, so the global is read afresh for the
        // setProperty rather than reused from before it.
        ev::Persistent globalThis(ev::globalValue("globalThis").value);
        ev::registerGlobal("self", globalThis.get());
        ev::setProperty(globalThis.get(), "self", globalThis.get());
    }

    auto jsPostMessage = [this](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::undefined();
        // The transfer list works in this direction too: ArrayBuffers,
        // ImageBitmaps and Meshes listed here move to the main realm.
        std::vector<ev::Persistent> transfers = collectTransferList(args);
        auto msg = std::make_unique<Message>();
        const std::vector<Value> transferVals = currentValues(transfers);
        if (serializeMessage(args[0], transferVals, *msg)) {
            postToMain(std::move(msg));
        }
        return ev::undefined();
    };

    ev::setGlobalValue("onmessage", ev::undefined());
    ev::setGlobalValue("onerror", ev::undefined());
    ev::setGlobalFunction("postMessage", 2, jsPostMessage);
    ev::setGlobalFunction("close", 0, [this](Value, std::span<const Value>) -> Value {
        terminated_.store(true, std::memory_order_release);
        return ev::undefined();
    });

    auto jsAddEventListener = [&addWorkerListener](Value, std::span<const Value> args) -> Value {
        if (args.size() >= 2 && ev::isString(args[0]) && ev::isFunction(args[1])) {
            addWorkerListener(ev::toUtf8(args[0]), args[1]);
        }
        return ev::undefined();
    };
    auto jsRemoveEventListener = [&removeWorkerListener](Value, std::span<const Value> args) -> Value {
        if (args.size() >= 2 && ev::isString(args[0]) && ev::isFunction(args[1])) {
            removeWorkerListener(ev::toUtf8(args[0]), args[1]);
        }
        return ev::undefined();
    };
    ev::setGlobalFunction("addEventListener", 2, jsAddEventListener);
    ev::setGlobalFunction("removeEventListener", 2, jsRemoveEventListener);
    for (const char* name : {"addEventListener", "removeEventListener"}) {
        // Both lookups may allocate, so each lands in a root before the
        // other is made, and the receiver is read last.
        ev::Persistent fn(ev::globalValue(name).value);
        ev::Persistent gt(ev::globalValue("globalThis").value);
        ev::setProperty(gt.get(), name, fn.get());
    }

    namespace bk = brokit::api;
    bk::installModuleRegistry();
    bk::installConsole();
    bk::installTimers();
    bk::installURL();
    bk::installCrypto();
    bk::installSubtleCrypto();
    bk::installEncoding();
    bk::installTreeWalker();
    bk::installAbortController();
    bk::installStructuredClone();
    bk::installBlob();
    bk::installURLObject();
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
    {
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            Value f = ev::getProperty(gt.value, "fetch");
            if (!ev::isUndefined(f)) ev::registerGlobal("fetch", f);
        }
    }
    bk::installXMLHttpRequest();
    bk::installCompression();
    bk::installBase64();
    bk::installEventTarget();
    bk::installMessageChannel();
    bk::installEvents();
    bk::installUtil();
    bk::installBuffer();
    {
        // registerGlobal allocates, so globalThis is rooted for the second
        // read rather than reused raw.
        ev::GlobalValue gtv = ev::globalValue("globalThis");
        if (gtv.found && ev::isObject(gtv.value)) {
            ev::Persistent gt(gtv.value);
            Value buf = ev::getProperty(gt.get(), "Buffer");
            if (!ev::isUndefined(buf)) ev::registerGlobal("Buffer", buf);
            Value sc = ev::getProperty(gt.get(), "structuredClone");
            if (!ev::isUndefined(sc)) ev::registerGlobal("structuredClone", sc);
        }
    }
    bk::installNet();
    bk::installNetJS();
    bk::installWebSocketServerJS();
    bk::installRequire();
    auto* eng = hostEngine();
    if (eng) {
        for (const auto& [prefix, target] : eng->assetMounts().mounts()) {
            brokit::api::addFsPrefixMount(prefix, target);
        }
        if (!eng->appDir().empty()) {
            brokit::api::addFsBasePath(eng->appDir());
        }
    }
    // `bro`: bro.net / bro.net.sync over this thread's own subscriber, the
    // sibling compute APIs over this thread's own classes, bro.server over
    // the control above (host_bro_root.cpp); the loop below polls them.
    installWorkerBroRoot();
    // The worker global's prototype chain, as the window's (host_global_proto.cpp):
    // String(self) is "[object DedicatedWorkerGlobalScope]".
    installGlobalPrototype("DedicatedWorkerGlobalScope");

    // unhandledrejection / rejectionhandled at `self`: bronze reports on this
    // thread's own drains, and the loop below flushes after each of them.
    {
        RejectionSink sink;
        sink.what = "worker unhandledrejection";
        sink.queueFlush = [] {};  // the loop polls rejectionEventsPending()
        sink.global = [] {
            ev::GlobalValue gt = ev::globalValue("globalThis");
            return (gt.found && ev::isObject(gt.value)) ? gt.value : ev::undefined();
        };
        sink.dispatch = [&workerListeners](Value evtV) {
            ev::Persistent evt(evtV);
            ev::Persistent typeV(ev::getProperty(evt.get(), "type"));
            const std::string type = ev::toUtf8(typeV.get());
            std::vector<ev::Persistent> fns;
            for (const auto& l : workerListeners) {
                if (l.type == type && ev::isFunction(l.fn.get())) fns.emplace_back(l.fn.get());
            }
            for (auto& fn : fns) {
                Value arg = evt.get();
                ev::CallResult r = ev::call(fn.get(), ev::undefined(), std::span<const Value>(&arg, 1));
                if (r.thrown) {
                    LOG_ERROR("[bronze:worker %s listener] uncaught %s", type.c_str(),
                              thrownValueText(r.value).c_str());
                }
            }
        };
        sink.onReported = [] { setTestFailure(true); };
        installRejectionTracking(std::move(sink));
    }
    auto flushRejections = [] {
        for (int i = 0; i < 8 && rejectionEventsPending(); ++i) {
            flushRejectionEvents();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        }
    };

    std::filesystem::path resolvedPath = scriptPath_;
    if (!resolvedPath.is_absolute() && !basePath_.empty()) {
        resolvedPath = std::filesystem::path(basePath_) / scriptPath_;
    }
    std::error_code ec;
    resolvedPath = std::filesystem::weakly_canonical(resolvedPath, ec);

    std::string scriptCode;
    {
        std::ifstream ifs(resolvedPath, std::ios::binary);
        if (ifs.is_open()) {
            scriptCode.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
    }

    if (scriptCode.empty()) {
        LOG_ERROR("worker: empty or missing script: %s", resolvedPath.string().c_str());
        dispatchWorkerError("worker: empty or missing script: " + resolvedPath.string(), resolvedPath.string(), 0);
    } else {
        std::regex re(R"((^|[^\w$.])onmessage\s*=)");
        if (std::regex_search(scriptCode, re)) {
            scriptCode = std::regex_replace(scriptCode, re, "$1self.onmessage = ");
        }
        std::regex reErr(R"((^|[^\w$.])onerror\s*=)");
        if (std::regex_search(scriptCode, reErr)) {
            scriptCode = std::regex_replace(scriptCode, reErr, "$1self.onerror = ");
        }

        bronze::eval::EvalOptions opts;
        opts.filename = resolvedPath.string();
        opts.entryResolvesAs = resolvedPath;
        // This thread's own registry: bronze's is per-thread, so this is
        // exactly the set the installs above put in for this worker —
        // self, postMessage, close, onmessage, brokit, the image, noise and
        // math globals, `bro`, the sibling classes (Mesh, GpuTensor,
        // FloraWorld, LMModel, ...) — and nothing the main realm has that a
        // worker does not.
        opts.hostGlobals = registeredHostGlobals();
        if (mounts_) {
            for (const auto& [prefix, target] : mounts_->mounts()) {
                opts.moduleRoots.push_back({prefix, std::filesystem::path(target)});
            }
        }
        opts.retainSource = true;
        // Every worker running this script runs one compiled program, each
        // from fresh module data (bronze EvalOptions::shareAcrossThreads):
        // the second worker skips the compile and starts on the code the
        // first one has tiered up.
        opts.shareAcrossThreads = true;

        auto hasAwaitStmt = [](const std::string& code) {
            size_t i = 0;
            while (i < code.size()) {
                while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) i++;
                if (i + 5 <= code.size() && code.compare(i, 5, "await") == 0) {
                    char next = (i + 5 < code.size()) ? code[i + 5] : '\0';
                    if (next == ' ' || next == '\t' || next == '(') return true;
                }
                while (i < code.size() && code[i] != '\n') i++;
                if (i < code.size() && code[i] == '\n') i++;
            }
            return false;
        };

        if (hasAwaitStmt(scriptCode)) {
            scriptCode = "(async () => {\n" + scriptCode +
                         "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); });\n";
        }

        auto res = bronze::eval::evalScript(scriptCode, opts);
        if (res.thrown) {
            // thrownValueText, not toUtf8: a thrown Error is an object, and
            // toUtf8 of an object is a hard error in the embed API.
            std::string errStr = thrownValueText(res.value);
            if (errStr.find("await") != std::string::npos) {
                std::string wrapped = "(async () => {\n" + scriptCode +
                                      "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); });\n";
                res = bronze::eval::evalScript(wrapped, opts);
            }
        }

        if (res.thrown) {
            std::string errStr;
            int lineno = 0;
            // The thrown value is read three times, with allocating calls
            // between: root it.
            ev::Persistent thrown(res.value);
            if (thrown.get().isObject()) {
                Value st = ev::getProperty(thrown.get(), "stack");
                if (!ev::isUndefined(st) && !ev::isNull(st)) {
                    errStr = ev::toUtf8(st);
                } else {
                    Value msg = ev::getProperty(thrown.get(), "message");
                    if (!ev::isUndefined(msg) && !ev::isNull(msg)) {
                        errStr = ev::toUtf8(msg);
                    }
                }
                Value ln = ev::getProperty(thrown.get(), "lineNumber");
                if (ev::isNumber(ln)) lineno = satCast<int>(ev::toDouble(ln));
            }
            if (errStr.empty()) {
                errStr = thrownValueText(thrown.get());
            }
            LOG_ERROR("worker script execution failed for %s: %s", resolvedPath.string().c_str(), errStr.c_str());
            dispatchWorkerError(errStr, resolvedPath.string(), lineno);
        }
    }

    ev::Persistent fetchTick(ev::globalValue("__brokit_fetch_tick").value);
    ev::Persistent wsTick(ev::globalValue("__brokit_ws_tick").value);
    ev::Persistent timersTick(ev::globalValue("__brokit_tick_timers").value);
    ev::Persistent fetchHasPending(ev::globalValue("__brokit_fetch_has_pending").value);

    while (!terminated_.load(std::memory_order_relaxed)) {
        Value gtVal = ev::globalValue("globalThis").value;
        if (ev::isObject(gtVal)) {
            // Rooted: the first getProperty may allocate (a getter, a lazy
            // namespace), and the second read would then use a dead address.
            ev::Persistent gt(gtVal);
            Value curOnmessage = ev::getProperty(gt.get(), "onmessage");
            if (ev::isFunction(curOnmessage)) {
                workerOnmessage.set(curOnmessage);
            }
            Value curOnerror = ev::getProperty(gt.get(), "onerror");
            if (ev::isFunction(curOnerror)) {
                workerOnerror.set(curOnerror);
            }
        }

        std::deque<std::unique_ptr<Message>> batch;
        {
            std::lock_guard<std::mutex> lock(toWorkerMutex_);
            batch.swap(toWorkerQueue_);
        }

        for (auto& msg : batch) {
            ev::Persistent dataRoot(deserializeMessage(*msg));
            ev::Persistent evObjRoot(makeMessageEvent(dataRoot));

            Value cb = workerOnmessage.get();
            if (ev::isFunction(cb)) {
                ev::Persistent cbRoot(cb);
                Value event = evObjRoot.get();
                ev::CallResult r = ev::call(cbRoot.get(), ev::undefined(), std::span<const Value>(&event, 1));
                if (r.thrown) {
                    std::string errStr = thrownValueText(r.value);
                    dispatchWorkerError(errStr, scriptPath_, 0);
                }
            }

            std::vector<ev::Persistent> msgListeners;
            for (const auto& l : workerListeners) {
                if (l.type == "message" && ev::isFunction(l.fn.get())) {
                    msgListeners.emplace_back(l.fn.get());
                }
            }
            for (auto& lfn : msgListeners) {
                Value event = evObjRoot.get();
                ev::CallResult r = ev::call(lfn.get(), ev::undefined(), std::span<const Value>(&event, 1));
                if (r.thrown) {
                    std::string errStr = thrownValueText(r.value);
                    dispatchWorkerError(errStr, scriptPath_, 0);
                }
            }
        }

        double nextTimerMs = -1.0;
        double nowMs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (ev::isFunction(timersTick.get())) {
            Value nowVal = ev::fromDouble(nowMs);
            ev::CallResult res = ev::call(timersTick.get(), ev::undefined(), std::span<const Value>(&nowVal, 1));
            if (!res.thrown && ev::isNumber(res.value)) {
                nextTimerMs = ev::toDouble(res.value);
            }
        }
        if (ev::isFunction(fetchTick.get())) {
            ev::call(fetchTick.get(), ev::undefined(), {});
        }
        if (ev::isFunction(wsTick.get())) {
            ev::call(wsTick.get(), ev::undefined(), {});
        }
        // This thread's NetSubscriber: its connect/disconnect/message
        // callbacks fire here, into the dispatcher js/net.js registered.
        pollNet();
        // This thread's sibling async jobs (a model load or inference the
        // script started), delivered to the callbacks it registered.
        tickWorkerSiblingApis();
        if (ev::microtasksPending()) {
            ev::drainMicrotasks();
        }
        flushRejections();

        bool hasPendingWork = false;
        if (ev::isFunction(fetchHasPending.get())) {
            Value has = ev::call(fetchHasPending.get(), ev::undefined(), {}).value;
            if (ev::toBool(has)) hasPendingWork = true;
        }

        {
            std::unique_lock<std::mutex> lock(toWorkerMutex_);
            if (!toWorkerQueue_.empty()) continue;
            // A script that set bro.server.tickrate asked for that cadence;
            // otherwise the idle wait is short enough for a reply to feel
            // immediate and long enough not to spin.
            const double hz = tickRateHz.load(std::memory_order_relaxed);
            std::chrono::milliseconds wait;
            if (hz > 0.0) {
                wait = std::chrono::milliseconds(static_cast<long long>(std::max(1.0, 1000.0 / hz)));
            } else if (hasPendingWork) {
                wait = std::chrono::milliseconds(5);
            } else if (nextTimerMs < 0.0) {
                wait = std::chrono::milliseconds(500);
            } else {
                long long t = static_cast<long long>(std::clamp(nextTimerMs, 1.0, 500.0));
                wait = std::chrono::milliseconds(t);
            }
            toWorkerCv_.wait_for(lock, wait);
        }
    }

    // This thread's sibling jobs still in flight, joined while the realm
    // their callbacks belong to is alive.
    shutdownWorkerSiblingApis();
    // Before workerListeners (which the sink's dispatch names) goes out of
    // scope, and while this thread's Persistent slots still exist.
    uninstallRejectionTracking();
    // The subscriber goes back to the service (its connections close) and
    // the dispatcher Persistent is freed while this thread's slots exist.
    releaseNetState();
    setWorkerServerControl(nullptr);
    alive_.store(false, std::memory_order_release);
}

static thread_local HostClass g_workerClass;

static void hostWorkerDtor(void* p) {
    auto* w = static_cast<WorkerInstance*>(p);
    {
        std::lock_guard<std::mutex> lock(s_workersMutex);
        auto it = std::find(s_activeWorkers.begin(), s_activeWorkers.end(), w);
        if (it != s_activeWorkers.end()) s_activeWorkers.erase(it);
    }
    delete w;
}

static WorkerInstance* getWorker(Value v) {
    if (!ev::isObject(v)) return nullptr;
    return static_cast<WorkerInstance*>(ev::handleData(v));
}

static std::string percentDecode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// The Worker's scriptURL as a path WorkerInstance loads. `new URL('./w.js',
// import.meta.url)` names a file: URL (import.meta.url is the module's file
// URL); the page's own base is bro://app/; anything else is a path, relative
// to the app directory as before. False (with `err`) for another scheme.
static bool workerScriptPath(const std::string& url, std::string& path, std::string& err) {
    auto startsWith = [&](const char* p) { return url.rfind(p, 0) == 0; };
    std::string rest;
    if (startsWith("file://")) {
        rest = url.substr(7);
        // file:///D:/x → D:/x; file:///home/x → /home/x; file://host/x is a
        // UNC path the loader has no business reading.
        if (rest.empty() || rest[0] != '/') { err = "unsupported file URL host"; return false; }
        if (rest.size() >= 3 && std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
            rest.erase(0, 1);
        }
    } else if (startsWith("bro://app/")) {
        rest = url.substr(10);
    } else {
        const size_t colon = url.find(':');
        const size_t slash = url.find('/');
        // A scheme is letters before a ':' with no '/' ahead of it, and is
        // longer than a Windows drive letter.
        if (colon != std::string::npos && colon > 1 &&
            (slash == std::string::npos || colon < slash)) {
            err = "unsupported URL scheme '" + url.substr(0, colon) + ":'";
            return false;
        }
        path = url;
        return true;
    }
    const size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos) rest.erase(cut);
    path = percentDecode(rest);
    return true;
}

} // namespace

void drainWorkerMessages() {
    std::vector<WorkerInstance*> workers;
    {
        std::lock_guard<std::mutex> lock(s_workersMutex);
        workers = s_activeWorkers;
    }
    for (auto* w : workers) {
        if (w) {
            w->drainMessagesToMain();
        }
    }
}

void terminateAllWorkers() {
    std::vector<WorkerInstance*> workers;
    {
        std::lock_guard<std::mutex> lock(s_workersMutex);
        workers = s_activeWorkers;
    }
    for (auto* w : workers) {
        if (w) {
            w->terminate();
        }
    }
}

void installWorkerGlobals(engine::Engine& engine) {
    g_workerClass.install("Worker", 1,
        [&engine](Value, std::span<const Value> a) -> Value {
            // scriptURL: a string, or a URL (anything stringifying through
            // its href), as `new Worker(new URL('./w.js', import.meta.url))`.
            std::string url;
            if (!a.empty() && ev::isString(a[0])) {
                url = ev::toUtf8(a[0]);
            } else if (!a.empty() && ev::isObject(a[0])) {
                Value href = ev::getProperty(a[0], "href");
                if (ev::isString(href)) url = ev::toUtf8(href);
            }
            if (url.empty()) {
                return ev::throwTypeError("new Worker(scriptURL) requires a script path or URL");
            }
            std::string script, err;
            if (!workerScriptPath(url, script, err)) {
                return ev::throwTypeError("new Worker('" + url + "'): " + err);
            }
            std::string base = engine.appDir().empty() ? "." : engine.appDir();
            auto* w = new WorkerInstance(script, base, &engine.assetMounts());
            {
                std::lock_guard<std::mutex> lock(s_workersMutex);
                s_activeWorkers.push_back(w);
            }
            w->start();
            return g_workerClass.make(w, hostWorkerDtor);
        },
        [](ObjectBuilder& proto) {
            proto.accessor("onmessage",
                [](Value thisVal, std::span<const Value>) -> Value {
                    auto* w = getWorker(thisVal);
                    return w ? w->getOnMessage() : ev::undefined();
                },
                [](Value thisVal, std::span<const Value> a) -> Value {
                    if (auto* w = getWorker(thisVal)) {
                        w->setOnMessage(a.empty() ? ev::undefined() : a[0]);
                    }
                    return ev::undefined();
                });

            proto.accessor("onerror",
                [](Value thisVal, std::span<const Value>) -> Value {
                    auto* w = getWorker(thisVal);
                    return w ? w->getOnError() : ev::undefined();
                },
                [](Value thisVal, std::span<const Value> a) -> Value {
                    if (auto* w = getWorker(thisVal)) {
                        w->setOnError(a.empty() ? ev::undefined() : a[0]);
                    }
                    return ev::undefined();
                });

            proto.def("addEventListener", 2, [](Value thisVal, std::span<const Value> a) -> Value {
                auto* w = getWorker(thisVal);
                if (!w || a.size() < 2 || !ev::isString(a[0])) return ev::undefined();
                w->addEventListener(ev::toUtf8(a[0]), a[1]);
                return ev::undefined();
            });

            proto.def("removeEventListener", 2, [](Value thisVal, std::span<const Value> a) -> Value {
                auto* w = getWorker(thisVal);
                if (!w || a.size() < 2 || !ev::isString(a[0])) return ev::undefined();
                w->removeEventListener(ev::toUtf8(a[0]), a[1]);
                return ev::undefined();
            });

            proto.def("postMessage", 1, [](Value thisVal, std::span<const Value> a) -> Value {
                auto* w = getWorker(thisVal);
                if (!w) return ev::undefined();
                if (a.empty()) return ev::undefined();

                // Rooted one by one: each element read can move the ones
                // before it.
                std::vector<ev::Persistent> transfers = collectTransferList(a);

                auto msg = std::make_unique<Message>();
                const std::vector<Value> transferVals = currentValues(transfers);
                if (!serializeMessage(a[0], transferVals, *msg)) {
                    return ev::undefined();
                }
                w->postToWorker(std::move(msg));
                return ev::undefined();
            });

            proto.def("terminate", 0, [](Value thisVal, std::span<const Value>) -> Value {
                if (auto* w = getWorker(thisVal)) {
                    w->terminate();
                }
                return ev::undefined();
            });
        });

    // `postMessage` and `close` are worker-scope names (DedicatedWorkerGlobal
    // Scope), and the compiler's manifest lists them, so the MAIN realm has to
    // answer them too or a compiled read of either is a fatal miss. The answer
    // is a no-op — but only when nothing real holds the name. A host global is
    // an own property of globalThis, so a stub registered over the window's
    // `close` replaces `window.close` itself, and a child window that calls it
    // never closes. This install runs last (installPlatformExtensions is at the
    // foot of installDomGlobals), so the guard is what keeps the window's own
    // members from being the ones a stub lands on.
    if (!ev::globalValue("postMessage").found) {
        ev::registerGlobal("postMessage", ev::makeFunction(
            [](Value, std::span<const Value>) { return ev::undefined(); }, 1, "postMessage"));
    }
    if (!ev::globalValue("close").found) {
        ev::registerGlobal("close", ev::makeFunction(
            [](Value, std::span<const Value>) { return ev::undefined(); }, 0, "close"));
    }
}

// Buffer and structuredClone used to be installed here; they come with the
// rest of brokit now (installBrokitGlobals, host_brokit.cpp), which runs
// before this.
void installPlatformExtensions(engine::Engine& engine) {
    installNoiseGlobals();
    installImageBitmapGlobals();
    installCustomElementsGlobals();
    installWorkerGlobals(engine);
}

} // namespace bro::bronze_host

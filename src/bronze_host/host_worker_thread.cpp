#include "bronze_host/host_worker_internal.h"
#include "bronze_host/app_aot.h"
#include "bronze_host/app_module.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_global_kinds.h"
#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_rejection_events.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_web_globals.h"
#include "engine/engine.h"
#include "eval/eval.h"
#include "util/log.h"
#include "util/object_url.h"
#include <api/api.h>
#include "embed/embed_access.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

void WorkerInstance::threadFunc() {
    // Where a worker's boot goes, for the one line logged once its script
    // has run: the env, brokit, bro's own globals, then compile + top level.
    using BootClock = std::chrono::steady_clock;
    const BootClock::time_point bootStart = BootClock::now();
    BootClock::time_point bootMarks[4] = {bootStart, bootStart, bootStart, bootStart};
    ensureSharedRuntimeEnv();
    const ProfilerThreadScope profiled(scriptPath_);  // bro.profiler {threads: 'workers'}
    installImageBitmapGlobals();
    installNoiseGlobals();
    // The bro.math classes BEFORE the roots, as in the main realm: the root
    // aliases their constructors (host_math_funcs.cpp). Per thread, like
    // ImageBitmap's.
    installMathGlobals();
    // VideoEncoder / GifEncoder over typed arrays: a worker renders frames and
    // encodes them without handing every frame to the page (host_video.cpp).
    installVideoGlobals();

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
        // Read at each dispatch, as an event handler attribute is.
        ev::Persistent cb(ev::undefined());
        if (Value gt = ev::globalValue("globalThis").value; ev::isObject(gt)) cb.set(ev::getProperty(gt, "onerror"));

        ev::Persistent errEvt(makeWorkerErrorEvent(msg, fn, line));

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

    // WorkerGlobalScope's self is a readonly attribute (the window's is
    // [Replaceable]; host_global_kinds.h).
    registerHostGlobal("self", ev::globalValue("globalThis").value, ev::GlobalKind::Readonly);

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

    // The handler attributes, null until the script sets them (a bare
    // `onmessage = f` is the global object's [[Set]]); the loop below reads
    // them off the global object.
    ev::setGlobalValue("onmessage", ev::null());
    ev::setGlobalValue("onerror", ev::null());
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

    bootMarks[0] = BootClock::now();
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
    markProcessBrowser();
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
    bk::installHttp();
    bk::installHttpJS();
    bk::installSqlite();
    bk::installSqliteJS();
    bk::installRequire();
    auto* eng = hostEngine();
    if (mounts_) {
        for (const auto& [prefix, target] : mounts_->mounts()) {
            brokit::api::addFsPrefixMount(prefix, target);
            brokit::api::addFetchPrefixMount(prefix, target);
        }
    } else if (eng) {
        for (const auto& [prefix, target] : eng->assetMounts().mounts()) {
            brokit::api::addFsPrefixMount(prefix, target);
            brokit::api::addFetchPrefixMount(prefix, target);
        }
    }
    const std::string& workerBase = !basePath_.empty() ? basePath_ : (eng ? eng->appDir() : basePath_);
    if (!workerBase.empty()) {
        brokit::api::addFsBasePath(workerBase);
        brokit::api::addFetchBasePath(workerBase);
        brokit::api::setRequireBasePath(std::filesystem::path(std::u8string(workerBase.begin(), workerBase.end())));
        brokit::api::setIndexedDBPath(workerBase);
    }
    // The page's `bro://app/` URLs are the app folder's files here too
    // (pixi's worker fetches its textures by the page's absolute URLs).
    if (eng && !eng->appDir().empty()) brokit::api::addFetchUrlMount("bro://app", eng->appDir());
    // `bro`: bro.net / bro.net.sync over this thread's own subscriber, the
    // sibling compute APIs over this thread's own classes, bro.server over
    // the control above (host_bro_root.cpp); the loop below polls them.
    bootMarks[1] = BootClock::now();
    installWorkerBroRoot();
    if (auto* host = hostEngine(); host && host->installWorkerHostBindings()) {
        host->installWorkerHostBindings()();
    }
    // The worker global's prototype chain, as the window's (host_global_proto.cpp):
    // String(self) is "[object DedicatedWorkerGlobalScope]".
    installGlobalPrototype("DedicatedWorkerGlobalScope");
    // DOMRect, DOMPoint, DOMMatrix, DOMQuad (js/geometry.js), as on the page.
    installGeometryModule();

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

    // importScripts(...urls): each script fetched and run in this global
    // scope, in order, before the call returns (HTML "import scripts into
    // worker global scope"); a file that cannot be read is a NetworkError and
    // a script's own throw propagates. URLs as `new Worker` takes them (file:,
    // bro://app/, a path), a relative one against the worker script's folder,
    // and blob: / data: URLs.
    const std::filesystem::path importBase = resolvedPath.parent_path();
    ev::setGlobalFunction("importScripts", 1, [this, importBase](Value, std::span<const Value> args) -> Value {
        std::vector<std::pair<std::string, std::string>> scripts;  // name, code
        for (const Value& a : args) {
            const std::string url = ev::toUtf8(a);
            std::string code;
            std::string name = url;
            std::vector<uint8_t> bytes;
            if (url.rfind("blob:", 0) == 0 || url.rfind("data:", 0) == 0) {
                if (!util::inlineURLBytes(url, bytes))
                    return ev::throwValue(hostMakeDomError("NetworkError", "importScripts: cannot load " + url));
                code.assign(bytes.begin(), bytes.end());
            } else {
                std::string path, err;
                if (!workerScriptPath(url, path, err))
                    return ev::throwValue(hostMakeDomError("NetworkError", "importScripts: " + url + ": " + err));
                std::filesystem::path full(std::u8string(path.begin(), path.end()));
                if (!full.is_absolute()) full = importBase / full;
                std::ifstream ifs(full, std::ios::binary);
                if (!ifs.is_open())
                    return ev::throwValue(hostMakeDomError("NetworkError", "importScripts: cannot load " + url));
                code.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                const std::u8string u = full.u8string();
                name.assign(u.begin(), u.end());
            }
            scripts.emplace_back(std::move(name), std::move(code));
        }
        for (const auto& [name, code] : scripts) {
            bronze::eval::EvalOptions o;
            o.filename = name;
            o.hostGlobals = registeredHostGlobals();
            if (mounts_) {
                for (const auto& [prefix, target] : mounts_->mounts())
                    o.moduleRoots.push_back({prefix, std::filesystem::path(target)});
            }
            o.retainSource = true;
            o.shareAcrossThreads = true;
            o.classicScript = true;  // importScripts' scripts are classic scripts
            applyCodeCacheOptions(o);
            auto res = bronze::eval::evalScript(code, o);
            if (res.thrown) return ev::throwValue(res.value);
        }
        return ev::undefined();
    });

    std::string scriptCode = inlineSource_;
    if (scriptCode.empty()) {
        std::ifstream ifs(resolvedPath, std::ios::binary);
        if (ifs.is_open()) {
            scriptCode.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
    }

    bronze::eval::EvalOptions opts;
    opts.filename = resolvedPath.string();
    opts.entryResolvesAs = resolvedPath;
    opts.hostGlobals = registeredHostGlobals();
    if (mounts_) {
        for (const auto& [prefix, target] : mounts_->mounts()) {
            opts.moduleRoots.push_back({prefix, std::filesystem::path(target)});
        }
    }
    opts.retainSource = true;
    opts.shareAcrossThreads = true;
    opts.classicScript = !moduleScript_;
    applyCodeCacheOptions(opts);

    if (aotBuild_) {
        // `--aot-build`: this realm is the one the unit is compiled against,
        // and the script does not run (app_aot.cpp, buildAppAot).
        if (!scriptCode.empty()) {
            aotBuild_->scriptRead = true;
            opts.feedbackInPath = aotBuild_->profilePath;
            if (aotBuild_->keepCurrent) {
                bronze::eval::LoadedModule existing = bronze::eval::loadModule(scriptCode, opts, aotBuild_->recordPath);
                if (existing.ok) {
                    aotBuild_->current = true;
                    aotBuild_->currentModule = existing.modulePath;
                }
            }
            if (!aotBuild_->current) {
                aotBuild_->result =
                    bronze::eval::buildModule(scriptCode, opts, aotBuild_->modulePath, aotBuild_->recordPath);
            }
        }
        terminated_.store(true, std::memory_order_release);
    } else if (scriptCode.empty()) {
        LOG_ERROR("worker: empty or missing script: %s", resolvedPath.string().c_str());
        dispatchWorkerError("worker: empty or missing script: " + resolvedPath.string(), resolvedPath.string(), 0);
    } else {
        // An installed app's worker script runs its ahead-of-time module, or
        // compiles to be harvested for one (app_aot.h, loadWorkerAotUnit). A
        // blob: or data: worker's text has no file to be the unit of.
        bootMarks[2] = BootClock::now();
        bronze::embed::CallResult res;
        std::optional<bronze::eval::LoadedModule> aot;
        if (auto* eng = hostEngine(); eng && inlineSource_.empty()) {
            aot = loadWorkerAotUnit(*eng, scriptCode, opts, moduleScript_, basePath_, workerAot_);
        }
        if (aot) {
            res = bronze::eval::runModule(*aot, opts);
        } else {
            auto compiled = bronze::eval::compileScript(scriptCode, opts);
            res = bronze::eval::runCompiledScript(std::move(compiled), opts);
        }
        bootMarks[3] = BootClock::now();
        auto ms = [](BootClock::time_point a, BootClock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        LOG_INFO("worker %s ready in %.1f ms (setup %.1f, brokit %.1f, bro %.1f, compile + top level %.1f)",
                 resolvedPath.filename().string().c_str(), ms(bootStart, bootMarks[3]),
                 ms(bootStart, bootMarks[0]), ms(bootMarks[0], bootMarks[1]), ms(bootMarks[1], bootMarks[2]),
                 ms(bootMarks[2], bootMarks[3]));

        if (res.thrown) {
            std::string errStr;
            int lineno = 0;
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
    ev::Persistent netTick(ev::globalValue("__brokit_net_tick").value);
    ev::Persistent fsWatchTick(ev::globalValue("__brokit_fs_watch_tick").value);
    ev::Persistent timersTick(ev::globalValue("__brokit_tick_timers").value);
    ev::Persistent timersNext(ev::globalValue("__brokit_next_timer").value);
    auto wallNowMs = [] {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    };
    ev::Persistent fetchHasPending(ev::globalValue("__brokit_fetch_has_pending").value);
    ev::Persistent wsHasPending(ev::globalValue("__brokit_ws_has_pending").value);
    ev::Persistent netHasPending(ev::globalValue("__brokit_net_has_pending").value);
    ev::Persistent fsWatchHasPending(ev::globalValue("__brokit_fs_watch_has_pending").value);
    ev::Persistent fsAsyncTick(ev::globalValue("__brokit_fs_async_tick").value);
    ev::Persistent fsAsyncHasPending(ev::globalValue("__brokit_fs_async_has_pending").value);

    while (!terminated_.load(std::memory_order_relaxed)) {
        std::deque<std::unique_ptr<Message>> batch;
        {
            std::lock_guard<std::mutex> lock(toWorkerMutex_);
            batch.swap(toWorkerQueue_);
        }

        for (auto& msg : batch) {
            ev::CallResult data = ev::catchThrow([&] { return deserializeMessage(*msg); });
            if (data.thrown) {
                LOG_ERROR("[bronze:worker] dropping a message that does not deserialize: %s",
                          thrownValueText(data.value).c_str());
                continue;
            }
            ev::Persistent dataRoot(data.value);
            ev::Persistent evObjRoot(makeWorkerMessageEvent(dataRoot));

            // The handler as it is now: an earlier message's handler may
            // have replaced it (Monaco's worker swaps onmessage on its first).
            Value gtVal = ev::globalValue("globalThis").value;
            Value cb = ev::isObject(gtVal) ? ev::getProperty(gtVal, "onmessage") : ev::undefined();
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
            if (ev::microtasksPending()) ev::drainMicrotasks();
        }

        const double nowMs = wallNowMs();
        if (ev::isFunction(timersTick.get())) {
            Value args[2] = {ev::fromDouble(nowMs), ev::fromDouble(1.0)};
            for (int fired = 0; fired < 256; ++fired) {
                ev::CallResult res = ev::call(timersTick.get(), ev::undefined(), std::span<const Value>(args, 2));
                if (ev::microtasksPending()) ev::drainMicrotasks();
                if (res.thrown || !ev::isNumber(res.value) || ev::toDouble(res.value) != 0.0) break;
            }
        }
        if (ev::isFunction(fetchTick.get())) {
            ev::call(fetchTick.get(), ev::undefined(), {});
        }
        if (ev::isFunction(wsTick.get())) {
            ev::call(wsTick.get(), ev::undefined(), {});
        }
        if (ev::isFunction(netTick.get())) {
            ev::call(netTick.get(), ev::undefined(), {});
        }
        if (ev::isFunction(fsWatchTick.get())) {
            ev::call(fsWatchTick.get(), ev::undefined(), {});
        }
        if (ev::isFunction(fsAsyncTick.get())) {
            ev::call(fsAsyncTick.get(), ev::undefined(), {});
        }
        pollNet();
        tickWorkerSiblingApis();
        // A grant taken back while the app runs: what this Worker holds open
        // under it closes here (embed/embed_access.h).
        bronze::embed::revalidateHeldAccess();
        // Every turn, pending microtasks or not: the checkpoint also runs the
        // deferred finalizers, among them the ones that free an ArrayBuffer's
        // external block, and a Worker whose handlers never await would
        // otherwise keep every buffer it was ever handed (a decode worker's
        // pixels, 24 MB a photo).
        ev::drainMicrotasks();
        flushRejections();

        bool hasPendingWork = false;
        auto checkPending = [&](const ev::Persistent& fn) {
            if (!hasPendingWork && ev::isFunction(fn.get())) {
                Value has = ev::call(fn.get(), ev::undefined(), {}).value;
                if (ev::toBool(has)) hasPendingWork = true;
            }
        };
        checkPending(fetchHasPending);
        checkPending(wsHasPending);
        checkPending(netHasPending);
        checkPending(fsWatchHasPending);
        checkPending(fsAsyncHasPending);

        double nextTimerMs = -1.0;
        if (ev::isFunction(timersNext.get())) {
            Value nowVal = ev::fromDouble(wallNowMs());
            ev::CallResult res = ev::call(timersNext.get(), ev::undefined(), std::span<const Value>(&nowVal, 1));
            if (!res.thrown && ev::isNumber(res.value)) nextTimerMs = ev::toDouble(res.value);
        }
        if (nextTimerMs == 0.0) continue;

        {
            std::unique_lock<std::mutex> lock(toWorkerMutex_);
            if (!toWorkerQueue_.empty()) continue;
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

    // A worker compiled to be harvested leaves its harvest while its realm
    // still runs (bronze's harvest is per thread; app_aot.h settleWorkerAot).
    if (workerAot_.harvesting || workerAot_.coldEntries) {
        if (workerAot_.harvesting) bronze::embed::writeInlineCacheHarvests();
        settleWorkerAot(workerAot_);
    }

    shutdownWorkerSiblingApis();
    brokit::api::uninstallFetch();
    brokit::api::uninstallWebSocket();
    brokit::api::uninstallNet();
    brokit::api::uninstallFSWatch();
    brokit::api::cleanupSqlite();
    brokit::api::cleanupIndexedDB();
    uninstallRejectionTracking();
    releaseNetState();
    setWorkerServerControl(nullptr);
    alive_.store(false, std::memory_order_release);
}

}  // namespace bro::bronze_host

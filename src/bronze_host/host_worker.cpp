#include "bronze_host/host_worker_msg.h"
#include "bronze_host/app_module.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include "eval/eval.h"
#include <api/api.h>

#include <atomic>
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

    void drainMessagesToMain(Value onmessageCb) {
        std::deque<std::unique_ptr<Message>> batch;
        {
            std::lock_guard<std::mutex> lock(toMainMutex_);
            batch.swap(toMainQueue_);
        }
        if (batch.empty() || !ev::isFunction(onmessageCb)) return;

        for (auto& msg : batch) {
            Value data = deserializeMessage(*msg);
            ObjectBuilder evObj;
            evObj.set("data", data);
            Value event = evObj.get();
            ev::call(onmessageCb, ev::undefined(), std::span<const Value>(&event, 1));
        }
    }

    bool isAlive() const { return alive_.load(std::memory_order_acquire); }

    void setOnMessage(Value cb) { onmessage_.set(cb); }
    Value getOnMessage() const { return onmessage_.get(); }

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
};

void WorkerInstance::threadFunc() {
    ensureSharedRuntimeEnv();

    installImageBitmapGlobals();
    installNoiseGlobals();

    ev::Persistent workerOnmessage;

    Value globalThis = ev::globalValue("globalThis").value;
    ev::registerGlobal("self", globalThis);
    ev::setProperty(globalThis, "self", globalThis);

    auto jsPostMessage = [this](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::undefined();
        Value data = args[0];
        std::span<const Value> transfers;
        auto msg = std::make_unique<Message>();
        if (serializeMessage(data, transfers, *msg)) {
            postToMain(std::move(msg));
        }
        return ev::undefined();
    };

    ev::setGlobalValue("onmessage", ev::undefined());
    ev::setGlobalFunction("postMessage", 1, jsPostMessage);
    ev::setGlobalFunction("close", 0, [this](Value, std::span<const Value>) -> Value {
        terminated_.store(true, std::memory_order_release);
        return ev::undefined();
    });

    brokit::api::installAll();
    auto* eng = hostEngine();
    if (eng) {
        for (const auto& [prefix, target] : eng->assetMounts().mounts()) {
            brokit::api::addFsPrefixMount(prefix, target);
        }
        if (!eng->appDir().empty()) {
            brokit::api::addFsBasePath(eng->appDir());
        }
    }

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
    } else {
        std::regex re(R"((^|[^\w$.])onmessage\s*=)");
        if (std::regex_search(scriptCode, re)) {
            scriptCode = std::regex_replace(scriptCode, re, "$1self.onmessage = ");
        }

        bronze::eval::EvalOptions opts;
        opts.filename = resolvedPath.string();
        opts.entryResolvesAs = resolvedPath;
        // This thread's own registry: bronze's is per-thread, so this is
        // exactly the set the installs above put in for this worker —
        // self, postMessage, close, onmessage, brokit, the image and noise
        // globals — and nothing the main realm has that a worker does not.
        opts.hostGlobals = registeredHostGlobals();
        if (mounts_) {
            for (const auto& [prefix, target] : mounts_->mounts()) {
                opts.moduleRoots.push_back({prefix, std::filesystem::path(target)});
            }
        }
        opts.retainSource = true;

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
            std::string errStr = ev::toUtf8(res.value);
            if (errStr.find("await") != std::string::npos) {
                std::string wrapped = "(async () => {\n" + scriptCode +
                                      "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); });\n";
                res = bronze::eval::evalScript(wrapped, opts);
            }
        }

        if (res.thrown) {
            std::string errStr;
            if (res.value.isObject()) {
                Value st = ev::getProperty(res.value, "stack");
                if (!ev::isUndefined(st) && !ev::isNull(st)) {
                    errStr = ev::toUtf8(st);
                } else {
                    Value msg = ev::getProperty(res.value, "message");
                    if (!ev::isUndefined(msg) && !ev::isNull(msg)) {
                        errStr = ev::toUtf8(msg);
                    }
                }
            }
            if (errStr.empty()) {
                errStr = ev::toUtf8(res.value);
            }
            LOG_ERROR("worker script execution failed for %s: %s", resolvedPath.string().c_str(), errStr.c_str());
        }
    }

    Value fetchTick = ev::globalValue("__brokit_fetch_tick").value;
    Value wsTick = ev::globalValue("__brokit_ws_tick").value;
    Value timersTick = ev::globalValue("__brokit_tick_timers").value;
    Value fetchHasPending = ev::globalValue("__brokit_fetch_has_pending").value;

    while (!terminated_.load(std::memory_order_relaxed)) {
        Value curOnmessage = ev::getProperty(globalThis, "onmessage");
        if (ev::isFunction(curOnmessage)) {
            workerOnmessage.set(curOnmessage);
        }

        std::deque<std::unique_ptr<Message>> batch;
        {
            std::lock_guard<std::mutex> lock(toWorkerMutex_);
            batch.swap(toWorkerQueue_);
        }

        for (auto& msg : batch) {
            Value data = deserializeMessage(*msg);
            ObjectBuilder evObj;
            evObj.set("data", data);
            Value event = evObj.get();
            if (ev::isFunction(workerOnmessage.get())) {
                ev::call(workerOnmessage.get(), ev::undefined(), std::span<const Value>(&event, 1));
            }
        }

        double nowMs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (ev::isFunction(timersTick)) {
            Value nowVal = ev::fromDouble(nowMs);
            ev::call(timersTick, ev::undefined(), std::span<const Value>(&nowVal, 1));
        }
        if (ev::isFunction(fetchTick)) {
            ev::call(fetchTick, ev::undefined(), {});
        }
        if (ev::isFunction(wsTick)) {
            ev::call(wsTick, ev::undefined(), {});
        }
        if (ev::microtasksPending()) {
            ev::drainMicrotasks();
        }

        bool hasPendingWork = false;
        if (ev::isFunction(fetchHasPending)) {
            Value has = ev::call(fetchHasPending, ev::undefined(), {}).value;
            if (ev::toBool(has)) hasPendingWork = true;
        }

        {
            std::unique_lock<std::mutex> lock(toWorkerMutex_);
            if (!toWorkerQueue_.empty()) continue;
            if (hasPendingWork) {
                toWorkerCv_.wait_for(lock, std::chrono::milliseconds(5));
            } else {
                toWorkerCv_.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
    }

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

} // namespace

void drainWorkerMessages() {
    std::vector<WorkerInstance*> workers;
    {
        std::lock_guard<std::mutex> lock(s_workersMutex);
        workers = s_activeWorkers;
    }
    for (auto* w : workers) {
        if (w) {
            w->drainMessagesToMain(w->getOnMessage());
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
            if (a.empty() || !ev::isString(a[0])) {
                return ev::throwTypeError("new Worker(scriptPath) requires a script path");
            }
            std::string script = ev::toUtf8(a[0]);
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

            proto.def("postMessage", 1, [](Value thisVal, std::span<const Value> a) -> Value {
                auto* w = getWorker(thisVal);
                if (!w) return ev::undefined();
                if (a.empty()) return ev::undefined();

                std::vector<Value> transfers;
                if (a.size() > 1 && ev::isObject(a[1])) {
                    Value lenV = ev::getProperty(a[1], "length");
                    if (ev::isNumber(lenV)) {
                        uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
                        for (uint32_t i = 0; i < len; ++i) {
                            transfers.push_back(ev::getElement(a[1], i));
                        }
                    }
                }

                auto msg = std::make_unique<Message>();
                if (!serializeMessage(a[0], std::span<const Value>(transfers.data(), transfers.size()), *msg)) {
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

    ev::registerGlobal("postMessage", ev::makeFunction([](Value, std::span<const Value>) { return ev::undefined(); }, 1, "postMessage"));
    ev::registerGlobal("close", ev::makeFunction([](Value, std::span<const Value>) { return ev::undefined(); }, 0, "close"));
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

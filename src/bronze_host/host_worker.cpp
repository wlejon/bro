#include "bronze_host/host_worker_internal.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_web_globals.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

static std::mutex s_workersMutex;
static std::vector<WorkerInstance*> s_activeWorkers;
static std::atomic<uint64_t> s_workerIdSeq{1};

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

static bool workerScriptPath(const std::string& url, std::string& path, std::string& err) {
    auto startsWith = [&](const char* p) { return url.rfind(p, 0) == 0; };
    std::string rest;
    if (startsWith("file://")) {
        rest = url.substr(7);
        if (rest.empty() || rest[0] != '/') { err = "unsupported file URL host"; return false; }
        if (rest.size() >= 3 && std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
            rest.erase(0, 1);
        }
    } else if (startsWith("bro://app/")) {
        rest = url.substr(10);
    } else {
        const size_t colon = url.find(':');
        const size_t slash = url.find('/');
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

}  // namespace

void WorkerInstance::postToMain(std::unique_ptr<Message> msg) {
    {
        std::lock_guard<std::mutex> lock(toMainMutex_);
        toMainQueue_.push_back(std::move(msg));
    }
    util::wakeMainLoop();
}

void WorkerInstance::drainMessagesToMain() {
    const bool threadDone = !alive_.load(std::memory_order_acquire);
    std::deque<std::unique_ptr<Message>> batch;
    {
        std::lock_guard<std::mutex> lock(toMainMutex_);
        batch.swap(toMainQueue_);
    }
    struct UnpinAfter {
        WorkerInstance* w;
        bool done;
        ~UnpinAfter() { if (done) w->unpinWrapper(); }
    } unpinAfter{this, threadDone};
    if (batch.empty()) return;

    for (auto& msg : batch) {
        if (msg->isError) {
            ev::Persistent evt(makeWorkerErrorEvent(msg->errorMessage, msg->errorFilename,
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
            ev::CallResult data = ev::catchThrow([&] { return deserializeMessage(*msg); });
            if (data.thrown) {
                LOG_ERROR("[bronze:worker] dropping a message that does not deserialize: %s",
                          thrownValueText(data.value).c_str());
                continue;
            }
            ev::Persistent dataRoot(data.value);
            ev::Persistent evt(makeWorkerMessageEvent(dataRoot));

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

void drainWorkerMessages() {
    std::vector<WorkerInstance*> workers;
    {
        std::lock_guard<std::mutex> lock(s_workersMutex);
        workers = s_activeWorkers;
    }
    for (auto* w : workers) {
        {
            std::lock_guard<std::mutex> lock(s_workersMutex);
            if (std::find(s_activeWorkers.begin(), s_activeWorkers.end(), w) == s_activeWorkers.end()) {
                continue;
            }
        }
        w->drainMessagesToMain();
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
            w->unpinWrapper();
        }
    }
}

void installWorkerGlobals(engine::Engine& engine) {
    g_workerClass.install("Worker", 1,
        [&engine](Value, std::span<const Value> a) -> Value {
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
            uint64_t wid = s_workerIdSeq.fetch_add(1, std::memory_order_relaxed);
            auto* w = new WorkerInstance(script, base, &engine.assetMounts(), wid);
            {
                std::lock_guard<std::mutex> lock(s_workersMutex);
                s_activeWorkers.push_back(w);
            }
            w->start();
            w->pinWrapper(g_workerClass.make(w, hostWorkerDtor));
            return w->pinnedWrapper();
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
                    w->unpinWrapper();
                }
                return ev::undefined();
            });
        });

    if (!ev::globalValue("postMessage").found) {
        ev::registerGlobal("postMessage", ev::makeFunction(
            [](Value, std::span<const Value>) { return ev::undefined(); }, 1, "postMessage"));
    }
    if (!ev::globalValue("close").found) {
        ev::registerGlobal("close", ev::makeFunction(
            [](Value, std::span<const Value>) { return ev::undefined(); }, 0, "close"));
    }
}

void installPlatformExtensions(engine::Engine& engine) {
    installNoiseGlobals();
    installImageBitmapGlobals();
    installCustomElementsGlobals();
    installWorkerGlobals(engine);
}

}  // namespace bro::bronze_host

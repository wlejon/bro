#pragma once

#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_worker_msg.h"
#include "eval/eval.h"
#include "util/asset_mounts.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bro::bronze_host {

inline Value makeWorkerErrorEvent(const std::string& message, const std::string& filename, int lineno) {
    ObjectBuilder evt;
    evt.set("type", ev::fromUtf8("error"));
    evt.set("message", ev::fromUtf8(message));
    evt.set("filename", ev::fromUtf8(filename));
    evt.set("lineno", ev::fromDouble(lineno));
    return evt.get();
}

inline Value makeWorkerMessageEvent(const ev::Persistent& data) {
    ObjectBuilder evt;
    evt.set("type", ev::fromUtf8("message"));
    evt.set("data", data.get());
    return evt.get();
}

// `bro-headless --aot-build` builds a worker unit's module in a worker realm
// set up exactly as the worker's run sets it up (app_aot.cpp, buildAppAot):
// the thread compiles its script with this instead of running it, and ends.
struct WorkerAotBuild {
    std::string modulePath;
    std::string recordPath;
    std::string profilePath;
    bool keepCurrent = false;  // a module whose record is still current is kept
    // Out:
    bool scriptRead = false;
    bool current = false;      // kept: the module its record names
    std::string currentModule;
    bronze::eval::ModuleBuildResult result;
};

// A worker's run of its ahead-of-time unit (app_aot.h, loadWorkerAotUnit):
// worker thread only.
struct WorkerAotRun {
    std::string name;
    std::string record;
    std::string profile;
    std::string harvestOut;  // this instance's harvest, renamed onto `profile` when written
    std::string appDir;      // what the rebuild is of, kept for a worker ending in the engine's teardown
    std::string aotDir;
    bool harvesting = false;
    const uint64_t* coldEntries = nullptr;
    uint32_t coldEntryCount = 0;
    double startMs = 0.0;
};

class WorkerInstance {
public:
    // First, as host_class.h's handle tags are.
    const uint32_t tag = kHostWorkerTag;

    // `inlineSource`, when not empty, is the script's text (a blob: or data:
    // URL's); `scriptPath` then only names it. `moduleScript`: made with
    // {type: 'module'}; otherwise the script is a classic script, whose
    // top-level var and function declarations are properties of `self`.
    WorkerInstance(std::string scriptPath, std::string basePath, const util::AssetMounts* mounts, uint64_t workerId,
                   std::string inlineSource, bool moduleScript)
        : scriptPath_(std::move(scriptPath)),
          basePath_(std::move(basePath)),
          inlineSource_(std::move(inlineSource)),
          moduleScript_(moduleScript),
          mounts_(mounts),
          workerId_(workerId) {}

    ~WorkerInstance() {
        terminate();
    }

    uint64_t id() const { return workerId_; }

    void start() {
        alive_.store(true, std::memory_order_release);
        workerThread_ = std::thread(&WorkerInstance::threadFunc, this);
    }

    // Build mode (WorkerAotBuild): set before start(); the thread builds and
    // ends by itself, and waitForExit() joins it.
    void setAotBuild(WorkerAotBuild* build) { aotBuild_ = build; }
    void waitForExit() {
        if (workerThread_.joinable()) workerThread_.join();
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

    // The page that made it is going (a reload): the thread stops, and what it
    // already sent, its handlers (the old page's closures) and the wrapper's
    // pin go with it, so no reply reaches code whose module is unloaded.
    void detachFromPage() {
        terminate();
        {
            std::lock_guard<std::mutex> lock(toMainMutex_);
            toMainQueue_.clear();
        }
        onmessage_.set(ev::undefined());
        onerror_.set(ev::undefined());
        listeners_.clear();
        unpinWrapper();
    }

    void postToWorker(std::unique_ptr<Message> msg) {
        if (terminated_.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(toWorkerMutex_);
        toWorkerQueue_.push_back(std::move(msg));
        toWorkerCv_.notify_one();
    }

    void postToMain(std::unique_ptr<Message> msg);
    void drainMessagesToMain();

    bool isAlive() const { return alive_.load(std::memory_order_acquire); }

    void pinWrapper(Value wrapper) { self_.set(wrapper); }
    void unpinWrapper() { self_.set(ev::undefined()); }
    Value pinnedWrapper() const { return self_.get(); }

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

    struct Listener {
        std::string type;
        ev::Persistent fn;
    };

private:
    void threadFunc();

    std::string scriptPath_;
    std::string basePath_;
    std::string inlineSource_;
    bool moduleScript_ = false;
    const util::AssetMounts* mounts_ = nullptr;
    uint64_t workerId_ = 0;
    WorkerAotBuild* aotBuild_ = nullptr;
    WorkerAotRun workerAot_;

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
    ev::Persistent self_;
    std::vector<Listener> listeners_;
};

// The file a Worker or importScripts URL names: a file:// or bro://app/ URL's
// path (percent-decoded, query and fragment dropped), or a path as given.
// False, with `err`, for another scheme.
bool workerScriptPath(const std::string& url, std::string& path, std::string& err);

}  // namespace bro::bronze_host

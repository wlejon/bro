#pragma once

#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_worker_msg.h"
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

class WorkerInstance {
public:
    // First, as host_class.h's handle tags are.
    const uint32_t tag = kHostWorkerTag;

    WorkerInstance(std::string scriptPath, std::string basePath, const util::AssetMounts* mounts, uint64_t workerId)
        : scriptPath_(std::move(scriptPath)),
          basePath_(std::move(basePath)),
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
    ev::Persistent self_;
    std::vector<Listener> listeners_;
};

}  // namespace bro::bronze_host

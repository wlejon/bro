#include "platform/control_socket.h"

#include "platform/control_protocol.h"

#include <brolink/loop.h>
#include <brolink/paths.h>
#include <brolink/wire.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bro::platform {

namespace {

// How long the loop sleeps with nothing to do: wake() ends it sooner.
constexpr int kIdleMs = 1000;

}  // namespace

struct ControlSocket::Impl final : brolink::LoopHandler {
    // Made by start(), dropped by stop() (under `mu`, which reply() wakes it under).
    std::unique_ptr<brolink::EventLoop> loop;
    std::string address;
    std::thread thread;
    std::atomic<bool> running{false};

    // Loop thread only: each connection's frame splitter.
    std::map<brolink::ConnId, brolink::wire::MessageSplitter> conns;

    std::mutex mu;  // guards the two queues below and the ready pipe
    std::vector<Request> requests;
    struct Reply {
        uint64_t conn;
        std::string message;
    };
    std::vector<Reply> replies;
    // POSIX: readable while requests wait for take(), for a caller asleep in poll().
    int readyRead = -1, readyWrite = -1;

    void on_accept(brolink::ConnId id) override { conns.emplace(id, brolink::wire::MessageSplitter(control::kMaxMessage)); }

    void on_data(brolink::ConnId id, const char* data, size_t n) override {
        auto it = conns.find(id);
        if (it == conns.end()) return;
        brolink::wire::MessageSplitter& split = it->second;
        split.feed(data, n);
        brolink::wire::MessageSplitter::Message m;
        std::vector<Request> got;
        while (split.next(m)) {
            Request req;
            req.conn = id;
            if (m.type != control::kRequest || !control::decodeRequest(m.payload, req.id, req.argv))
                req.parseError = "bad request (message type " + std::to_string(m.type) + ")";
            got.push_back(std::move(req));
        }
        if (!got.empty()) {
            std::lock_guard<std::mutex> lk(mu);
            if (requests.empty()) signalReady();
            for (auto& r : got) requests.push_back(std::move(r));
        }
        // A frame that cannot be resynchronised: the connection is done.
        if (split.error()) loop->close(id, false);
    }

    void on_closed(brolink::ConnId id) override { conns.erase(id); }

    void signalReady() {
#if !defined(_WIN32)
        if (readyWrite >= 0) {
            char one = 1;
            (void)::write(readyWrite, &one, 1);
        }
#endif
    }

    void run() {
        std::vector<Reply> out;
        while (running.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lk(mu);
                out.swap(replies);
            }
            for (Reply& r : out)
                if (conns.count(r.conn)) loop->write(r.conn, r.message);
            out.clear();
            loop->run_once(kIdleMs);
        }
        loop->close_listener();
    }
};

ControlSocket::ControlSocket() : impl_(std::make_unique<Impl>()) {}
ControlSocket::~ControlSocket() { stop(); }

bool ControlSocket::running() const { return impl_->running.load(std::memory_order_acquire); }
const std::string& ControlSocket::path() const { return impl_->address; }

std::string ControlSocket::runtimeDir() { return brolink::runtime_dir(control::kApp); }

bool ControlSocket::start(const std::string& name, std::string* why) {
    if (running()) return true;
    std::string err;
    const std::string address = brolink::local_address(control::kApp, name, &err);
    if (address.empty()) {
        if (why) *why = err;
        return false;
    }
    auto loop = brolink::EventLoop::create(*impl_);
    bool inUse = false;
    if (!loop->listen(address, err, inUse)) {
        if (why) *why = inUse ? "another process is serving " + address : err;
        return false;
    }
#if !defined(_WIN32)
    int ready[2];
    if (::pipe(ready) != 0) {
        if (why) *why = "pipe failed";
        return false;  // the loop's destructor removes the endpoint
    }
    for (int fd : ready) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    impl_->readyRead = ready[0];
    impl_->readyWrite = ready[1];
#endif
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->loop = std::move(loop);
        impl_->conns.clear();
    }
    impl_->address = address;
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
}

void ControlSocket::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->loop->wake();
    if (impl_->thread.joinable()) impl_->thread.join();
    std::lock_guard<std::mutex> lk(impl_->mu);
    // Its connections close and the endpoint goes with the loop.
    impl_->loop.reset();
    impl_->conns.clear();
    impl_->replies.clear();
#if !defined(_WIN32)
    ::close(impl_->readyRead);
    ::close(impl_->readyWrite);
    impl_->readyRead = impl_->readyWrite = -1;
#endif
}

std::vector<ControlSocket::Request> ControlSocket::take() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<Request> out;
    out.swap(impl_->requests);
#if !defined(_WIN32)
    if (impl_->readyRead >= 0) {
        char buf[64];
        while (::read(impl_->readyRead, buf, sizeof(buf)) > 0) {
        }
    }
#endif
    return out;
}

int ControlSocket::readyFd() const { return impl_ ? impl_->readyRead : -1; }

void ControlSocket::reply(uint64_t conn, uint64_t id, bool ok, std::string payload) {
    std::string message = control::encodeReply(id, ok, payload);
    // A reply after stop() (from a command's worker thread) has nowhere to go.
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->loop) return;
    impl_->replies.push_back({conn, std::move(message)});
    impl_->loop->wake();
}

}  // namespace bro::platform

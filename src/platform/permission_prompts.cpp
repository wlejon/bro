#include "platform/permission_prompts.h"

#include "platform/desktop_platform.h"
#include "platform/window_system.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

#include <brolink/loop.h>
#include <brolink/paths.h>
#include <brolink/stream.h>
#include <brolink/wire.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bro::platform::prompts {

namespace {

constexpr const char* kApp = "bro-prompt";
constexpr uint16_t kAsk = 1;      // app -> shell: Question
constexpr uint16_t kAnswer = 2;   // shell -> app: u8 (1 allow, 0 deny)
constexpr size_t kMaxMessage = 64u << 10;
constexpr size_t kMaxField = 4096;
// How long the loop sleeps with nothing to do: wake() ends it sooner.
constexpr int kIdleMs = 1000;

std::atomic<int> g_serving{0};

std::string encodeQuestion(const Question& q) {
    brolink::wire::Writer w;
    w.str(q.appName);
    w.str(q.appId);
    w.str(q.identity);
    w.str(q.dir);
    w.str(q.capability);
    w.str(q.what.size() > kMaxField ? q.what.substr(0, kMaxField) : q.what);
    return brolink::wire::make_message(kAsk, w.data());
}

bool decodeQuestion(std::string_view payload, Question& q) {
    brolink::wire::Reader r(payload);
    q.appName = r.str_max(kMaxField);
    q.appId = r.str_max(kMaxField);
    q.identity = r.str_max(kMaxField);
    q.dir = r.str_max(kMaxField);
    q.capability = r.str_max(64);
    q.what = r.str_max(kMaxField);
    return r.done();
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

std::string endpointName(const std::string& scratchHome) {
    if (!scratchHome.empty()) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(scratchHome)));
        return std::string("scratch-") + hex;
    }
#ifdef _WIN32
    // The pipe namespace is the machine's: one shell per logon session.
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session)) return "session-" + std::to_string(session);
#endif
    // POSIX: the endpoint lives in the user's own runtime directory.
    return "session";
}

bool servingInProcess() { return g_serving.load(std::memory_order_acquire) > 0; }

AskResult ask(const std::string& endpoint, const Question& q) {
    // The shell's own uses never wait on itself.
    if (servingInProcess()) return AskResult::NoShell;
    std::string err;
    const std::string address = brolink::local_address(kApp, endpoint, &err);
    if (address.empty()) return AskResult::NoShell;
    bool notRunning = false;
    std::unique_ptr<brolink::Stream> stream = brolink::connect_local(address, &err, &notRunning);
    if (!stream) {
        if (!notRunning) LOG_WARN("permissions: the desktop shell's prompts are unreachable: %s", err.c_str());
        return AskResult::NoShell;
    }
    if (!stream->write(encodeQuestion(q))) return AskResult::NoShell;
    LOG_INFO("permissions: asking the desktop shell whether %s may use %s", q.appName.c_str(), q.capability.c_str());

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int answer = -1;
    std::thread reader([&] {
        brolink::wire::MessageSplitter split(kMaxMessage);
        char buf[512];
        int got = -1;
        for (bool more = true; more;) {
            const size_t n = stream->read(buf, sizeof buf);
            if (n == 0) break;
            split.feed(buf, n);
            brolink::wire::MessageSplitter::Message m;
            while (more && split.next(m)) {
                if (m.type != kAnswer) continue;
                brolink::wire::Reader r(m.payload);
                const uint8_t a = r.u8();
                if (r.done() && a <= 1) got = a;
                more = false;
            }
            if (split.error()) break;
        }
        std::lock_guard<std::mutex> lk(mu);
        answer = got;
        done = true;
        cv.notify_all();
    });

    // Modal, like the dialog it stands in for: the script that asked is on
    // the stack. The window's events are pumped (the compositor does not
    // call it hung); a quit ends the wait with no answer.
    const bool pump = !desktop::isHeadless();
    bool shut = false;
    std::unique_lock<std::mutex> lk(mu);
    while (!done) {
        if (!shut && util::interrupted()) {
            shut = true;
            stream->shutdown();
        }
        lk.unlock();
        if (pump) pumpEvents();
        lk.lock();
        cv.wait_for(lk, std::chrono::milliseconds(16), [&] { return done; });
    }
    lk.unlock();
    reader.join();
    stream->shutdown();
    if (answer == 1) return AskResult::Allow;
    if (answer == 0) return AskResult::Deny;
    LOG_INFO("permissions: the desktop shell gave no answer about %s", q.capability.c_str());
    return AskResult::NoAnswer;
}

// ---- the shell's end ---------------------------------------------------------------

struct Server::Impl final : brolink::LoopHandler {
    std::unique_ptr<brolink::EventLoop> loop;   // made by start(), dropped by stop()
    std::thread thread;
    std::atomic<bool> running{false};

    // Loop thread only: each connection's splitter, and whether it asked.
    struct Conn {
        brolink::wire::MessageSplitter split{kMaxMessage};
        uint64_t pid = 0;
        bool asked = false;
    };
    std::map<brolink::ConnId, Conn> conns;

    std::mutex mu;                    // guards everything below
    std::vector<Event> events;        // for take()
    std::set<uint64_t> outstanding;   // asked, not answered, dropped or gone
    struct Out {
        uint64_t id;
        int answer;   // 1 allow, 0 deny, -1 drop
    };
    std::vector<Out> out;             // for the loop thread

    void on_accept(brolink::ConnId id) override { conns[id].pid = loop->peer_pid(id); }

    void on_data(brolink::ConnId id, const char* data, size_t n) override {
        auto it = conns.find(id);
        if (it == conns.end()) return;
        Conn& c = it->second;
        c.split.feed(data, n);
        brolink::wire::MessageSplitter::Message m;
        bool bad = c.split.error();
        while (!bad && c.split.next(m)) {
            Event e;
            if (c.asked || m.type != kAsk || !decodeQuestion(m.payload, e.question)) {
                bad = true;
                break;
            }
            c.asked = true;
            e.kind = Event::Kind::Request;
            e.id = id;
            e.peerPid = c.pid;
            {
                std::lock_guard<std::mutex> lk(mu);
                outstanding.insert(id);
                events.push_back(std::move(e));
            }
            util::wakeMainLoop();
        }
        // One question per connection, well formed, or it is done.
        if (bad) loop->close(id, false);
    }

    void on_closed(brolink::ConnId id) override {
        conns.erase(id);
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (outstanding.erase(id)) {
                Event e;
                e.kind = Event::Kind::Cancel;
                e.id = id;
                events.push_back(std::move(e));
                cancelled = true;
            }
        }
        if (cancelled) util::wakeMainLoop();
    }

    void run() {
        std::vector<Out> todo;
        while (running.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lk(mu);
                todo.swap(out);
            }
            for (const Out& o : todo) {
                if (!conns.count(o.id)) continue;
                if (o.answer >= 0) {
                    brolink::wire::Writer w;
                    w.u8(static_cast<uint8_t>(o.answer));
                    loop->write(o.id, brolink::wire::make_message(kAnswer, w.data()));
                }
                loop->close(o.id, /*flush_first=*/o.answer >= 0);
            }
            todo.clear();
            loop->run_once(kIdleMs);
        }
        loop->close_listener();
    }

    bool settle(uint64_t id, int answer) {
        std::lock_guard<std::mutex> lk(mu);
        if (!loop || !outstanding.erase(id)) return false;
        out.push_back({id, answer});
        loop->wake();
        return true;
    }
};

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

bool Server::running() const { return impl_->running.load(std::memory_order_acquire); }

bool Server::start(const std::string& endpoint, std::string* why) {
    if (running()) return true;
    std::string err;
    const std::string address = brolink::local_address(kApp, endpoint, &err);
    if (address.empty()) {
        if (why) *why = err;
        return false;
    }
    auto loop = brolink::EventLoop::create(*impl_);
    bool inUse = false;
    if (!loop->listen(address, err, inUse)) {
        if (why) *why = inUse ? "another shell serves the prompts at " + address : err;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->loop = std::move(loop);
        impl_->events.clear();
        impl_->outstanding.clear();
        impl_->out.clear();
    }
    impl_->conns.clear();
    impl_->running.store(true, std::memory_order_release);
    g_serving.fetch_add(1, std::memory_order_acq_rel);
    impl_->thread = std::thread([this] { impl_->run(); });
    LOG_INFO("permissions: serving app permission prompts at %s", address.c_str());
    return true;
}

void Server::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    impl_->loop->wake();
    if (impl_->thread.joinable()) impl_->thread.join();
    std::unique_ptr<brolink::EventLoop> loop;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        loop = std::move(impl_->loop);
        impl_->outstanding.clear();   // what closes now is no one's cancellation
        impl_->out.clear();
    }
    // Its connections close (each asker: no answer) and the endpoint goes
    // with the loop (on_closed may run in here: the lock is not held).
    loop.reset();
    impl_->conns.clear();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->events.clear();
    }
    g_serving.fetch_sub(1, std::memory_order_acq_rel);
}

std::vector<Server::Event> Server::take() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<Event> got;
    got.swap(impl_->events);
    return got;
}

bool Server::respond(uint64_t id, bool allow) { return impl_->settle(id, allow ? 1 : 0); }
bool Server::drop(uint64_t id) { return impl_->settle(id, -1); }

}  // namespace bro::platform::prompts

// ---------------------------------------------------------------------------
// Asking another bro (an app's endpoint), off the engine thread
// ---------------------------------------------------------------------------

struct Fetch {
    std::mutex mu;
    std::unique_ptr<brolink::Stream> stream;
    bool cancelled = false;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string payload;
    int window = 0;
};
using FetchPtr = std::shared_ptr<Fetch>;

FetchPtr startFetch(const std::string& endpoint, std::vector<std::string> argv, int window) {
    auto f = std::make_shared<Fetch>();
    f->window = window;
    std::thread([f, endpoint, argv = std::move(argv)]() {
        std::string err;
        auto stream = brolink::connect_local(brolink::local_address(control::kApp, endpoint), &err);
        if (!stream) {
            f->payload = "cannot reach " + endpoint + ": " + err;
            f->done.store(true, std::memory_order_release);
            return;
        }
        brolink::Stream* raw = stream.get();
        {
            std::lock_guard<std::mutex> lk(f->mu);
            if (f->cancelled) {
                f->payload = "timed out";
                f->done.store(true, std::memory_order_release);
                return;
            }
            f->stream = std::move(stream);
        }
        bool got = false;
        if (raw->write(control::encodeRequest(1, argv))) {
            brolink::wire::MessageSplitter split{control::kMaxMessage};
            brolink::wire::MessageSplitter::Message m;
            char buf[65536];
            while (!got) {
                while (!got && split.next(m)) {
                    uint64_t id = 0;
                    bool ok = false;
                    std::string payload;
                    if (m.type == control::kReply && control::decodeReply(m.payload, id, ok, payload) && id == 1) {
                        f->ok = ok;
                        f->payload = std::move(payload);
                        got = true;
                    }
                }
                if (got || split.error()) break;
                const size_t r = raw->read(buf, sizeof(buf));
                if (r == 0) break;
                split.feed(buf, r);
            }
        }
        if (!got && f->payload.empty()) f->payload = endpoint + " closed the connection";
        f->done.store(true, std::memory_order_release);
    }).detach();
    return f;
}

void cancelFetch(Fetch& f) {
    std::lock_guard<std::mutex> lk(f.mu);
    f.cancelled = true;
    if (f.stream && !f.done.load(std::memory_order_acquire)) f.stream->shutdown();
}

// Calls `then` on the engine thread once every fetch has answered, or at
// `timeoutMs` (the rest cancelled, reading as failed).
void whenAll(ControlServer& s, std::vector<FetchPtr> fetches, double timeoutMs, std::function<void()> then) {
    ControlServer* server = &s;
    const double start = s.clockMs();
    auto tick = [server, fetches, timeoutMs, start, then]() {
        bool all = true;
        for (const auto& f : fetches)
            if (!f->done.load(std::memory_order_acquire)) all = false;
        if (!all && server->clockMs() - start < timeoutMs) return false;
        for (const auto& f : fetches)
            if (!f->done.load(std::memory_order_acquire)) cancelFetch(*f);
        then();
        return true;
    };
    if (!tick()) s.addTicker(tick);
}

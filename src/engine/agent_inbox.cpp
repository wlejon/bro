#include "engine/agent_inbox.h"

#include "engine/engine.h"
#include "util/json_out.h"
#include "util/log.h"
#include "util/user_dirs.h"

#include <brolink/paths.h>
#include <brolink/random.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace bro::engine {

namespace inbox = platform::inbox;

namespace {

constexpr size_t kMaxText = 1u << 20;           // a request's or a reply's text
constexpr size_t kMaxProgressText = 4000;
constexpr size_t kMaxProgress = 1000;           // lines kept per request
constexpr size_t kMaxQueue = 1000;              // undelivered requests per agent
constexpr size_t kKeepFinished = 256;           // finished requests kept for status/result
constexpr size_t kKeepFinishedBytes = 256u << 20;
constexpr size_t kKeepEvents = 512;
constexpr size_t kLogTextBytes = 4000;          // text quoted in a log line
constexpr uintmax_t kLogRotateBytes = 8u << 20;

std::filesystem::path fsPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// At most `n` bytes of `s`, never cutting a UTF-8 sequence.
std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t end = n;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end) + "...";
}

bool validAgentName(const std::string& n) {
    if (n.empty() || n.size() > 64 || n == "." || n == "..") return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string randomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    if (!brolink::random_bytes(raw.data(), raw.size())) {
        // Not a secret (the session is the credential): only distinct.
        const uint64_t t = brolink::unix_time_ms() ^ (brolink::current_pid() << 20);
        for (size_t i = 0; i < bytes; ++i) raw[i] = static_cast<char>(t >> (8 * (i % 8)));
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

size_t filesBytes(const std::vector<inbox::File>& files) {
    size_t n = 0;
    for (const auto& f : files) n += f.bytes.size();
    return n;
}

void ok(const AgentInbox::Reply& r, std::string payload, std::string bundle = {}) {
    r(true, std::move(payload), std::move(bundle));
}
void fail(const AgentInbox::Reply& r, std::string why) { r(false, std::move(why), {}); }

}  // namespace

AgentInbox::AgentInbox(Clock clock, std::string logPath)
    : clock_(std::move(clock)), logPath_(std::move(logPath)), instance_(randomHex(2)) {}

AgentInbox::~AgentInbox() {
    // A headless engine's log is its own temp file (defaultLogPath).
    if (logPath_.find("bro-inbox-headless-") != std::string::npos) {
        std::error_code ec;
        std::filesystem::remove(fsPath(logPath_), ec);
        std::filesystem::remove(fsPath(logPath_ + ".1"), ec);
    }
}

std::string AgentInbox::defaultLogPath(Engine& engine) {
    if (const char* env = std::getenv("BRO_INBOX_LOG"); env && *env) return env;
    if (engine.displayMode() == DisplayMode::Headless) {
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec);
        const std::string name = "bro-inbox-headless-" + std::to_string(brolink::current_pid()) + ".jsonl";
        const auto p = dir / name;
        const std::u8string u8 = p.u8string();
        return std::string(u8.begin(), u8.end());
    }
    const auto p = fsPath(util::userDataDir()) / "agent-inbox.jsonl";
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// ---------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------

AgentInbox::Agent* AgentInbox::byToken(const std::string& token) {
    if (token.empty()) return nullptr;
    for (auto& [name, a] : agents_)
        if (a.token == token) return &a;
    return nullptr;
}

AgentInbox::Agent* AgentInbox::byName(const std::string& name) {
    auto it = agents_.find(name);
    return it == agents_.end() ? nullptr : &it->second;
}

AgentInbox::Request* AgentInbox::find(const std::string& id) {
    auto it = requests_.find(id);
    return it == requests_.end() ? nullptr : it->second.get();
}

bool AgentInbox::live(const Agent& a) const { return a.poll || clock_() - a.lastSeenMs <= kLeaseMs; }

bool AgentInbox::admits(const Sender&, const Agent&, std::string*) const {
    // Today the session is the credential: whoever reaches the endpoint (the
    // user's own socket, or an ssh login as the user through the remote) may
    // ask any agent anything. Per-agent keys and capability lists go here:
    // look up the sender's key, check the capability the agent asks for.
    return true;
}

const char* AgentInbox::stateName(State s) {
    switch (s) {
        case State::Queued: return "queued";
        case State::Working: return "working";
        case State::Done: return "done";
        case State::Failed: return "failed";
        case State::Cancelled: return "cancelled";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

std::string AgentInbox::originJson(const ControlOrigin& o) {
    util::JsonOut j;
    j.beginObject();
    j.key("via").string(o.via);
    if (!o.user.empty()) j.key("user").string(o.user);
    if (!o.sshClient.empty()) j.key("sshClient").string(o.sshClient);
    if (!o.client.empty()) j.key("client").string(o.client);
    if (o.pid) j.key("pid").integer(static_cast<int64_t>(o.pid));
    j.endObject();
    return j.take();
}

std::string AgentInbox::senderJson(const Sender& s) {
    util::JsonOut j;
    j.beginObject();
    j.key("agent").string(s.agent);
    if (!s.origin.empty()) j.key("origin").string(s.origin);
    j.key("via").string(s.via.via);
    if (!s.via.user.empty()) j.key("user").string(s.via.user);
    if (!s.via.sshClient.empty()) j.key("sshClient").string(s.via.sshClient);
    if (!s.via.client.empty()) j.key("client").string(s.via.client);
    if (s.via.pid) j.key("pid").integer(static_cast<int64_t>(s.via.pid));
    j.endObject();
    return j.take();
}

std::string AgentInbox::filesJson(const std::vector<inbox::File>& files) {
    util::JsonOut j;
    j.beginArray();
    for (const auto& f : files) {
        j.beginObject();
        j.key("name").string(f.name);
        j.key("size").integer(static_cast<int64_t>(f.bytes.size()));
        j.endObject();
    }
    j.endArray();
    return j.take();
}

std::string AgentInbox::statusJson(const Request& r, size_t since) const {
    util::JsonOut j;
    j.beginObject();
    j.key("id").string(r.id);
    j.key("to").string(r.to);
    j.key("state").string(stateName(r.state));
    j.key("from").raw(senderJson(r.from));
    j.key("asked").integer(r.askedUnix);
    j.key("elapsedMs").integer(static_cast<int64_t>((r.finished() ? r.finishedMs : clock_()) - r.askedMs));
    j.key("request").string(r.text);
    j.key("requestFiles").raw(filesJson(r.files));
    j.key("progress").beginArray();
    for (size_t i = std::min(since, r.progress.size()); i < r.progress.size(); ++i) {
        j.beginObject();
        j.key("i").integer(static_cast<int64_t>(i));
        j.key("atMs").integer(static_cast<int64_t>(r.progress[i].atMs));
        j.key("text").string(r.progress[i].text);
        j.endObject();
    }
    j.endArray();
    j.key("progressNext").integer(static_cast<int64_t>(r.progress.size()));
    if (r.state == State::Done) {
        j.key("text").string(r.answer);
        j.key("files").raw(filesJson(r.answerFiles));
    } else if (r.finished()) {
        j.key("error").string(r.answer);
    }
    j.endObject();
    return j.take();
}

std::string AgentInbox::requestEventJson(const Request& r) const {
    util::JsonOut j;
    j.beginObject();
    j.key("type").string("request");
    j.key("id").string(r.id);
    j.key("to").string(r.to);
    j.key("text").string(r.text);
    j.key("files").raw(filesJson(r.files));
    j.key("from").raw(senderJson(r.from));
    j.key("asked").integer(r.askedUnix);
    j.endObject();
    return j.take();
}

// ---------------------------------------------------------------------------
// the audit log and the event feed
// ---------------------------------------------------------------------------

void AgentInbox::audit(const std::string& event, const Request* r, const std::string& text,
                       const std::vector<inbox::File>* files, const Agent* agent, const ControlOrigin* by) {
    if (logPath_.empty()) return;
    util::JsonOut j;
    j.beginObject();
    j.key("t").integer(static_cast<int64_t>(brolink::unix_time_ms()));
    j.key("event").string(event);
    if (r) {
        j.key("id").string(r->id);
        j.key("to").string(r->to);
        j.key("from").raw(senderJson(r->from));
        j.key("state").string(stateName(r->state));
    }
    if (agent) {
        j.key("agent").string(agent->name);
        if (!agent->about.empty()) j.key("about").string(agent->about);
    }
    if (by) j.key("by").raw(originJson(*by));
    if (!text.empty()) j.key("text").string(clip(text, kLogTextBytes));
    if (files && !files->empty()) j.key("files").raw(filesJson(*files));
    j.endObject();
    std::string line = j.take();
    line += '\n';

    const auto path = fsPath(logPath_);
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > kLogRotateBytes && !ec) {
        std::filesystem::rename(path, fsPath(logPath_ + ".1"), ec);
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        static bool warned = false;
        if (!warned) LOG_WARN("agent inbox: cannot write the audit log %s", logPath_.c_str());
        warned = true;
        return;
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void AgentInbox::emit(const std::string& kind, const Request* r, const std::string& text, const std::string& to,
                      const std::string& from) {
    Event e;
    e.seq = nextEvent_++;
    e.atUnix = static_cast<int64_t>(brolink::unix_time_ms());
    e.kind = kind;
    e.text = clip(text, 200);
    if (r) {
        e.id = r->id;
        e.to = r->to;
        e.from = r->from.agent;
        e.state = stateName(r->state);
    } else {
        e.to = to;
        e.from = from;
    }
    events_.push_back(std::move(e));
    while (events_.size() > kKeepEvents) events_.pop_front();
    if (eventWaiters_.empty()) return;
    std::vector<Waiter> waiters;
    waiters.swap(eventWaiters_);
    for (Waiter& w : waiters) events(w.since, 0, std::move(w.reply));
}

// ---------------------------------------------------------------------------
// serving
// ---------------------------------------------------------------------------

void AgentInbox::registerAgent(const std::string& name, const std::string& about, bool replace,
                               const ControlOrigin& by, const Reply& reply) {
    if (!validAgentName(name)) return fail(reply, "an agent name is 1 to 64 of A-Z a-z 0-9 . _ -");
    std::deque<std::string> carried;
    if (Agent* old = byName(name)) {
        if (live(*old) && !replace) {
            std::string who = old->by.via;
            if (old->by.pid) who += ", pid " + std::to_string(old->by.pid);
            return fail(reply, "agent '" + name + "' is registered (" + who +
                                   "); --replace takes the name over");
        }
        // What it had not been handed yet goes to the new one; what it was
        // working on fails.
        carried.swap(old->queue);
        drop(name, "agent '" + name + "' was replaced before it answered");
    }
    Agent& a = agents_[name];
    a.name = name;
    a.about = clip(about, 1000);
    a.token = randomHex(16);
    a.by = by;
    a.registeredMs = a.lastSeenMs = clock_();
    a.registeredUnix = static_cast<int64_t>(brolink::unix_time_ms());
    a.queue.swap(carried);
    audit("register", nullptr, {}, nullptr, &a, &by);
    emit("registered", nullptr, a.about, name, name);

    util::JsonOut j;
    j.beginObject();
    j.key("name").string(name);
    j.key("token").string(a.token);
    j.key("leaseMs").integer(static_cast<int64_t>(kLeaseMs));
    j.key("pollMs").integer(static_cast<int64_t>(kMaxPollMs));
    j.endObject();
    ok(reply, j.take());
}

void AgentInbox::next(const std::string& token, double timeoutMs, Reply reply) {
    Agent* a = byToken(token);
    if (!a) return fail(reply, "not registered (the registration lapsed or was replaced: register again)");
    a->lastSeenMs = clock_();
    if (a->poll) {
        // A second poll replaces the first, which comes back empty.
        auto old = std::move(a->poll);
        ok(old->reply, "{\"type\":\"idle\"}");
        a = byToken(token);
        if (!a) return fail(reply, "not registered");
    }
    a->poll = std::make_unique<Waiter>();
    a->poll->deadline = clock_() + std::clamp(timeoutMs, 0.0, kMaxPollMs);
    a->poll->reply = std::move(reply);
    deliver(*a);
}

void AgentInbox::deliver(Agent& a) {
    if (!a.poll) return;
    if (!a.cancels.empty()) {
        const std::string id = a.cancels.front();
        a.cancels.pop_front();
        auto w = std::move(a.poll);
        a.lastSeenMs = clock_();
        ok(w->reply, "{\"type\":\"cancel\",\"id\":\"" + id + "\"}");
        return;
    }
    while (!a.queue.empty()) {
        Request* r = find(a.queue.front());
        a.queue.pop_front();
        if (!r || r->state != State::Queued) continue;  // cancelled while it waited
        r->state = State::Working;
        r->deliveredMs = clock_();
        auto w = std::move(a.poll);
        a.lastSeenMs = clock_();
        audit("deliver", r, {}, nullptr);
        emit("delivered", r, r->text);
        notifyStatus(*r);
        ok(w->reply, requestEventJson(*r), inbox::encode(r->files));
        return;
    }
}

void AgentInbox::progress(const std::string& token, const std::string& id, const std::string& text,
                          const Reply& reply) {
    Agent* a = byToken(token);
    if (!a) return fail(reply, "not registered (register again)");
    a->lastSeenMs = std::max(a->lastSeenMs, clock_());
    Request* r = find(id);
    if (!r || r->to != a->name) return fail(reply, "no request '" + id + "' for agent '" + a->name + "'");
    if (r->state != State::Working) return fail(reply, "request " + id + " is " + stateName(r->state));
    if (r->progress.size() >= kMaxProgress) r->progress.erase(r->progress.begin());
    r->progress.push_back({clock_() - r->askedMs, clip(text, kMaxProgressText)});
    audit("progress", r, text, nullptr);
    emit("progress", r, text);
    notifyStatus(*r);
    ok(reply, "{\"id\":\"" + id + "\",\"state\":\"working\"}");
}

void AgentInbox::answer(const std::string& token, const std::string& id, const std::string& text,
                        const std::string& bundle, bool failed, const Reply& reply) {
    Agent* a = byToken(token);
    if (!a) return fail(reply, "not registered (register again)");
    a->lastSeenMs = std::max(a->lastSeenMs, clock_());
    Request* r = find(id);
    if (!r || r->to != a->name) return fail(reply, "no request '" + id + "' for agent '" + a->name + "'");
    if (r->state == State::Cancelled) return fail(reply, "request " + id + " was cancelled");
    if (r->state != State::Working) return fail(reply, "request " + id + " is " + stateName(r->state));
    if (text.size() > kMaxText) return fail(reply, "the reply's text is over 1 MB: attach it as a file");
    std::vector<inbox::File> files;
    std::string why;
    if (!inbox::decode(bundle, files, &why)) return fail(reply, why);
    ++a->answered;
    finish(*r, failed ? State::Failed : State::Done, text, std::move(files));
    ok(reply, "{\"id\":\"" + id + "\",\"state\":\"" + stateName(r->state) + "\"}");
}

void AgentInbox::unregister(const std::string& token, const Reply& reply) {
    Agent* a = byToken(token);
    if (!a) return fail(reply, "not registered");
    const std::string name = a->name;
    drop(name, "agent '" + name + "' left before it answered");
    ok(reply, "{\"name\":\"" + name + "\",\"state\":\"left\"}");
}

void AgentInbox::drop(const std::string& name, const std::string& why) {
    auto it = agents_.find(name);
    if (it == agents_.end()) return;
    Agent gone = std::move(it->second);
    agents_.erase(it);
    audit("leave", nullptr, why, nullptr, &gone);
    emit("left", nullptr, why, name, name);
    if (gone.poll) ok(gone.poll->reply, "{\"type\":\"gone\"}");
    // Its queue went to a replacement, if any (registerAgent took it first).
    for (const std::string& id : gone.queue)
        if (Request* r = find(id); r && r->state == State::Queued) finish(*r, State::Failed, why, {});
    std::vector<Request*> held;
    for (auto& [id, r] : requests_)
        if (r->to == name && r->state == State::Working) held.push_back(r.get());
    for (Request* r : held) finish(*r, State::Failed, why, {});
}

void AgentInbox::finish(Request& r, State state, std::string answerText, std::vector<inbox::File> files) {
    r.state = state;
    r.finishedMs = clock_();
    r.answer = std::move(answerText);
    r.answerFiles = std::move(files);
    static const char* kAudit[] = {"", "", "reply", "fail", "cancel"};
    audit(kAudit[static_cast<int>(state)], &r, r.answer, &r.answerFiles);
    emit(stateName(state), &r, r.answer);
    finishedOrder_.push_back(r.id);
    const std::string id = r.id;
    notifyStatus(r);
    std::vector<Waiter> waiters;
    waiters.swap(r.resultWaiters);
    for (Waiter& w : waiters) result(id, 0, std::move(w.reply));
    prune();
}

void AgentInbox::notifyStatus(Request& r) {
    if (r.statusWaiters.empty()) return;
    std::vector<Waiter> waiters;
    waiters.swap(r.statusWaiters);
    const std::string id = r.id;
    for (Waiter& w : waiters) status(id, w.since, 0, std::move(w.reply));
}

void AgentInbox::prune() {
    size_t bytes = 0;
    for (const std::string& id : finishedOrder_)
        if (Request* r = find(id)) bytes += filesBytes(r->files) + filesBytes(r->answerFiles);
    while (!finishedOrder_.empty() && (finishedOrder_.size() > kKeepFinished || bytes > kKeepFinishedBytes)) {
        const std::string id = finishedOrder_.front();
        finishedOrder_.pop_front();
        auto it = requests_.find(id);
        if (it == requests_.end()) continue;
        bytes -= std::min(bytes, filesBytes(it->second->files) + filesBytes(it->second->answerFiles));
        requests_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// asking
// ---------------------------------------------------------------------------

void AgentInbox::agents(bool json, const Reply& reply) {
    const double now = clock_();
    util::JsonOut j;
    std::string text;
    j.beginArray();
    for (auto& [name, a] : agents_) {
        if (!live(a)) continue;
        size_t working = 0;
        for (auto& [id, r] : requests_)
            if (r->to == name && r->state == State::Working) ++working;
        j.beginObject();
        j.key("name").string(name);
        j.key("about").string(a.about);
        j.key("by").raw(originJson(a.by));
        j.key("since").integer(a.registeredUnix);
        j.key("asked").integer(static_cast<int64_t>(a.asked));
        j.key("answered").integer(static_cast<int64_t>(a.answered));
        j.key("queued").integer(static_cast<int64_t>(a.queue.size()));
        j.key("working").integer(static_cast<int64_t>(working));
        j.key("polling").boolean(a.poll != nullptr);
        j.key("idleMs").integer(static_cast<int64_t>(a.poll ? 0 : now - a.lastSeenMs));
        j.endObject();
        text += name + (a.about.empty() ? "" : "  " + a.about) + "  (" + std::to_string(a.answered) + "/" +
                std::to_string(a.asked) + " answered" + (working ? ", " + std::to_string(working) + " working" : "") +
                (a.queue.empty() ? "" : ", " + std::to_string(a.queue.size()) + " queued") + ")\n";
    }
    j.endArray();
    if (json) return ok(reply, j.take());
    ok(reply, text.empty() ? std::string("no agents registered (bro-ctl inbox serve NAME registers one)\n") : text);
}

void AgentInbox::ask(const std::string& to, const std::string& text, const std::string& bundle, const Sender& from,
                     double waitMs, Reply reply) {
    Agent* a = byName(to);
    if (!a || !live(*a)) return fail(reply, "no agent '" + to + "' in this session (bro-ctl agents lists them)");
    std::string why;
    if (!admits(from, *a, &why)) return fail(reply, why.empty() ? "not admitted" : why);
    if (text.size() > kMaxText) return fail(reply, "the request's text is over 1 MB: attach it as a file");
    if (a->queue.size() >= kMaxQueue) return fail(reply, "agent '" + to + "' has too many requests waiting");
    std::vector<inbox::File> files;
    if (!inbox::decode(bundle, files, &why)) return fail(reply, why);

    auto req = std::make_unique<Request>();
    Request& r = *req;
    r.id = instance_ + "-" + std::to_string(nextRequest_++);
    r.to = to;
    r.text = text;
    r.files = std::move(files);
    r.from = from;
    if (r.from.agent.empty()) r.from.agent = from.origin.empty() ? from.via.via : from.origin;
    r.askedMs = clock_();
    r.askedUnix = static_cast<int64_t>(brolink::unix_time_ms());
    requests_[r.id] = std::move(req);
    ++a->asked;
    a->queue.push_back(r.id);
    audit("ask", &r, r.text, &r.files);
    emit("asked", &r, r.text);
    const std::string id = r.id;
    deliver(*a);
    if (waitMs > 0) return result(id, waitMs, std::move(reply));
    Request* now = find(id);
    ok(reply, statusJson(*now, 0));
}

void AgentInbox::status(const std::string& id, size_t since, double waitMs, Reply reply) {
    Request* r = find(id);
    if (!r) return fail(reply, "no request '" + id + "' (finished long ago, or the host restarted)");
    if (waitMs > 0 && !r->finished() && r->progress.size() <= since) {
        r->statusWaiters.push_back({clock_() + waitMs, since, std::move(reply)});
        return;
    }
    ok(reply, statusJson(*r, since));
}

void AgentInbox::result(const std::string& id, double waitMs, Reply reply) {
    Request* r = find(id);
    if (!r) return fail(reply, "no request '" + id + "' (finished long ago, or the host restarted)");
    if (!r->finished() && waitMs > 0) {
        r->resultWaiters.push_back({clock_() + waitMs, 0, std::move(reply)});
        return;
    }
    ok(reply, statusJson(*r, 0), r->state == State::Done ? inbox::encode(r->answerFiles) : std::string());
}

void AgentInbox::cancel(const std::string& id, const Sender& by, const Reply& reply) {
    Request* r = find(id);
    if (!r) return fail(reply, "no request '" + id + "'");
    if (r->finished()) return ok(reply, "{\"id\":\"" + id + "\",\"state\":\"" + stateName(r->state) + "\"}");
    const bool held = r->state == State::Working;
    const std::string who = by.agent.empty() ? by.via.via : by.agent;
    finish(*r, State::Cancelled, "cancelled by " + who, {});
    if (held) {
        if (Agent* a = byName(r->to)) {
            a->cancels.push_back(id);
            deliver(*a);
        }
    }
    ok(reply, "{\"id\":\"" + id + "\",\"state\":\"cancelled\"}");
}

void AgentInbox::events(uint64_t since, double waitMs, Reply reply) {
    const bool any = !events_.empty() && events_.back().seq > since;
    if (!any && waitMs > 0) {
        eventWaiters_.push_back({clock_() + waitMs, static_cast<size_t>(since), std::move(reply)});
        return;
    }
    util::JsonOut j;
    j.beginObject();
    j.key("next").integer(static_cast<int64_t>(nextEvent_ - 1));
    j.key("events").beginArray();
    for (const Event& e : events_) {
        if (e.seq <= since) continue;
        j.beginObject();
        j.key("seq").integer(static_cast<int64_t>(e.seq));
        j.key("at").integer(e.atUnix);
        j.key("kind").string(e.kind);
        if (!e.id.empty()) j.key("id").string(e.id);
        j.key("to").string(e.to);
        if (!e.from.empty()) j.key("from").string(e.from);
        if (!e.state.empty()) j.key("state").string(e.state);
        j.key("text").string(e.text);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    ok(reply, j.take());
}

void AgentInbox::log(size_t limit, const std::string& id, const Reply& reply) {
    if (logPath_.empty()) return fail(reply, "this engine keeps no audit log");
    std::string text;
    for (const std::string& p : {logPath_ + ".1", logPath_}) {
        std::ifstream in(fsPath(p), std::ios::binary);
        if (in) text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<std::string> lines;
    const std::string needle = "\"id\":\"" + id + "\"";
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string_view line(text.data() + at, end - at);
        if (!line.empty() && (id.empty() || line.find(needle) != std::string_view::npos)) lines.emplace_back(line);
        at = end + 1;
    }
    if (limit && lines.size() > limit) lines.erase(lines.begin(), lines.end() - static_cast<ptrdiff_t>(limit));
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    ok(reply, out);
}

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------

void AgentInbox::tick() {
    if (agents_.empty() && eventWaiters_.empty()) return;
    const double now = clock_();
    std::vector<std::string> lapsed;
    for (auto& [name, a] : agents_) {
        if (a.poll && now >= a.poll->deadline) {
            auto w = std::move(a.poll);
            a.lastSeenMs = now;
            ok(w->reply, "{\"type\":\"idle\"}");
        } else if (!a.poll && now - a.lastSeenMs > kLeaseMs) {
            lapsed.push_back(name);
        }
    }
    for (const std::string& name : lapsed)
        drop(name, "agent '" + name + "' stopped polling (its registration lapsed)");

    // Held status and result polls on requests not yet finished.
    std::vector<std::pair<std::string, Waiter>> due;
    for (auto& [id, r] : requests_) {
        for (auto* list : {&r->statusWaiters, &r->resultWaiters}) {
            for (auto it = list->begin(); it != list->end();) {
                if (now >= it->deadline) {
                    due.emplace_back(id, std::move(*it));
                    it = list->erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    for (auto& [id, w] : due) {
        Request* r = find(id);
        if (r) ok(w.reply, statusJson(*r, w.since));
        else fail(w.reply, "no request '" + id + "'");
    }
    std::vector<Waiter> keep, expired;
    for (Waiter& w : eventWaiters_) (now >= w.deadline ? expired : keep).push_back(std::move(w));
    eventWaiters_.swap(keep);
    for (Waiter& w : expired) events(w.since, 0, std::move(w.reply));
}

}  // namespace bro::engine

// The agent view (docs/agent-view.md): what an agent sees and operates as
// text. `view` lists the windows and the elements of the focused one with
// handles (wN, N.M); actions name those handles and answer with the diff
// their effect made.
//
// Every bro serves its own document's view. The display server (an engine
// running a window manager: bro --drm, or the headless shell host) also
// lists the windows of the session and fetches each app's view from that
// app's own control endpoint (found by its process id), moving the app's
// rects into screen coordinates. Fetches run on threads; the engine thread
// only assembles, so a slow app costs the shell no frames.
//
// The text model (lines, revisions, diffs) is agent_view.h; the document
// walk is view_document.h.

#include "engine/agent_view.h"
#include "engine/app_runtime.h"
#include "engine/control.h"
#include "engine/control_helpers.h"
#include "engine/engine.h"
#include "engine/view_document.h"
#include "dom/document.h"
#include "dom/element.h"
#include "platform/control_protocol.h"

#include <brolink/paths.h>
#include <brolink/stream.h>
#include <brolink/wire.h>

#if BRO_WITH_COMPOSITOR
#include <brocompositor/api.h>
#include <brocompositor/window_manager.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace bro::engine {

namespace {

namespace control = platform::control;
using view::Item;
using view::Snapshot;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct WinInfo {
    int n = 0;
    uint64_t id = 0;
    uint32_t pid = 0;
    std::string appId, title;
    float x = 0, y = 0, w = 0, h = 0;  // screen CSS px
    bool focused = false, minimized = false;
};

// An app's last view as it sent it (its own coordinates, handles 0.M).
struct AppCache {
    uint64_t rev = 0;
    std::string key;
    Snapshot items;
};

struct ViewState {
    DocumentView doc;
    view::History history;
    // This document's elements, kept while it has not changed.
    struct {
        bool valid = false;
        uint64_t epoch = 0;
        const void* focus = nullptr;
        float vw = 0, vh = 0, sx = 0, sy = 0;
        std::string key;
        Snapshot items;
    } local;
    std::map<uint64_t, int> winNumbers;  // compositor window id -> N
    int nextWin = 1;
    std::map<uint64_t, AppCache> apps;   // by window id
    std::map<std::string, ControlServer::Handler> originals;
};

std::map<const ControlServer*, std::unique_ptr<ViewState>> g_states;

ViewState& stateOf(ControlServer& s) {
    auto& p = g_states[&s];
    if (!p) p = std::make_unique<ViewState>();
    return *p;
}

std::string appName(Engine& e) {
    std::string app = currentApp().id;
    if (app.empty()) app = std::filesystem::path(e.appDir()).filename().string();
    return app.empty() ? "bro" : app;
}

// ---------------------------------------------------------------------------
// Windows (the display server's)
// ---------------------------------------------------------------------------

// Whether this engine is the session's display (a compositor of its own,
// or the desktop shell over the system's) with a window manager to list.
bool aggregates(ControlServer& s) {
#if BRO_WITH_COMPOSITOR
    Engine& e = s.engine();
    if (e.shellCompositorSocket().empty() && !e.isDesktopShell()) return false;
    return brocompositor::api::getWindowManager() != nullptr;
#else
    (void)s;
    return false;
#endif
}

// Top of the stack first.
std::vector<WinInfo> desktopWindows(ControlServer& s) {
    std::vector<WinInfo> out;
#if BRO_WITH_COMPOSITOR
    auto wm = brocompositor::api::getWindowManager();
    if (!wm) return out;
    ViewState& st = stateOf(s);
    Engine& e = s.engine();
    // Compositor coordinates are output pixels; the view is in CSS px.
    const float px = e.viewportWidth() > 0 ? static_cast<float>(e.framePixelWidth()) / e.viewportWidth() : 1.0f;
    const auto& stack = wm->stacking();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        auto v = wm->window(*it);
        if (!v) continue;
        if (!v->shown && !v->snapshot.minimized) continue;  // another workspace
        WinInfo w;
        auto num = st.winNumbers.find(*it);
        if (num == st.winNumbers.end()) num = st.winNumbers.emplace(*it, st.nextWin++).first;
        w.n = num->second;
        w.id = *it;
        w.pid = v->snapshot.process_id;
        w.appId = v->snapshot.app_id;
        w.title = v->snapshot.title;
        w.x = static_cast<float>(v->snapshot.frame.x) / px;
        w.y = static_cast<float>(v->snapshot.frame.y) / px;
        w.w = static_cast<float>(v->snapshot.frame.width) / px;
        w.h = static_cast<float>(v->snapshot.frame.height) / px;
        w.focused = wm->focused() == *it;
        w.minimized = v->snapshot.minimized;
        out.push_back(std::move(w));
    }
#else
    (void)s;
#endif
    return out;
}

const WinInfo* findWin(const std::vector<WinInfo>& wins, int n) {
    for (const auto& w : wins)
        if (w.n == n) return &w;
    return nullptr;
}

Item windowLine(const WinInfo& w) {
    Item it;
    it.handle = "w" + std::to_string(w.n);
    it.role = w.appId.empty() ? "app" : w.appId;
    if (w.focused) it.flags = "*";
    it.name = w.title;
    if (w.minimized) it.extra = "min";
    it.hasRect = true;
    it.x = w.x;
    it.y = w.y;
    it.w = w.w;
    it.h = w.h;
    return it;
}

// This process's own window line: w0, the shell or the app.
Item ownLine(ControlServer& s, bool display, bool focused) {
    Engine& e = s.engine();
    Item it;
    it.handle = "w0";
    it.role = display ? "shell" : appName(e);
    if (focused) it.flags = "*";
    if (dom::Document* d = e.document()) it.name = d->title();
    it.hasRect = true;
    it.w = static_cast<float>(e.viewportWidth());
    it.h = static_cast<float>(e.viewportHeight());
    return it;
}

// The control endpoint a process serves (<app>-<pid>), else "".
std::string endpointForPid(uint32_t pid) {
    if (!pid) return {};
    const std::string suffix = "-" + std::to_string(pid);
    for (const auto& n : brolink::list_local(control::kApp))
        if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) return n;
    return {};
}

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

// ---------------------------------------------------------------------------
// Building a view
// ---------------------------------------------------------------------------

uint64_t parseRev(const std::string& s);

// What a view shows: whose elements, and a list page.
struct Scope {
    std::string key;
    std::set<int> windows;   // 0: this document
    int pageWindow = -1;
    uint32_t pageRoot = 0;
    int from = 0, count = 20;
};

// This document's elements as `window`'s, from the cache while nothing changed.
bool localElements(ControlServer& s, int window, uint32_t root, int from, int count, Snapshot& out) {
    Engine& e = s.engine();
    dom::Document* doc = e.document();
    if (!doc) return true;
    e.flushLayoutForRead(doc);
    ViewState& st = stateOf(s);
    auto& c = st.local;
    const std::string key = std::to_string(window) + ":" + std::to_string(root) + ":" + std::to_string(from) + ":" +
                            std::to_string(count);
    // The page's own viewport: the window less a menu bar (or a docked
    // inspector); its rects move by that inset into window coordinates.
    const float vw = static_cast<float>(e.contentWidth()), vh = static_cast<float>(e.contentHeight());
    const float insetX = static_cast<float>(e.contentLeft()), insetY = static_cast<float>(e.contentTop());
    const std::string cacheKey = key + "@" + std::to_string(e.contentLeft()) + "," + std::to_string(e.contentTop());
    const float sx = doc->viewportScrollX(), sy = doc->viewportScrollY();
    const void* focus = doc->activeElement();
    if (c.valid && c.epoch == doc->mutationEpoch() && c.focus == focus && c.vw == vw && c.vh == vh && c.sx == sx &&
        c.sy == sy && c.key == cacheKey) {
        out.insert(out.end(), c.items.begin(), c.items.end());
        return true;
    }
    DocumentView::Options opt;
    opt.window = window;
    opt.root = root;
    opt.from = from;
    opt.count = count;
    Snapshot items;
    const bool whole = st.doc.build(doc, DocumentView::Viewport{vw, vh, sx, sy}, opt, items);
    if (root && items.empty()) return false;
    for (Item& it : items) {
        it.x += insetX;
        it.y += insetY;
    }
    c.valid = true;
    c.epoch = doc->mutationEpoch();
    c.focus = focus;
    c.vw = vw;
    c.vh = vh;
    c.sx = sx;
    c.sy = sy;
    c.key = cacheKey;
    c.items = items;
    out.insert(out.end(), items.begin(), items.end());
    (void)whole;
    return true;
}

// An app's reply, moved into the desktop: handles 0.M become N.M, rects
// move from its viewport into its window's frame.
void placeAppItems(const WinInfo& w, const Snapshot& app, Snapshot& out) {
    float vw = 0, vh = 0;
    for (const Item& it : app)
        if (it.handle == "w0") {
            vw = it.w;
            vh = it.h;
        }
    const float sx = vw > 0 ? w.w / vw : 1.0f, sy = vh > 0 ? w.h / vh : 1.0f;
    const std::string prefix = std::to_string(w.n) + ".";
    for (const Item& it : app) {
        if (it.isWindow()) continue;
        Item m = it;
        m.handle = prefix + std::to_string(view::elementOf(it.handle));
        m.x = w.x + it.x * sx;
        m.y = w.y + it.y * sy;
        m.w = it.w * sx;
        m.h = it.h * sy;
        out.push_back(std::move(m));
    }
}

using ViewDone = std::function<void(Snapshot snap, std::vector<WinInfo> wins, std::string error)>;

void collectView(ControlServer& s, const Scope& sc, ViewDone done) {
    const bool display = aggregates(s);
    std::vector<WinInfo> wins = display ? desktopWindows(s) : std::vector<WinInfo>{};
    bool clientFocused = false;
    for (const auto& w : wins) clientFocused = clientFocused || w.focused;

    auto snap = std::make_shared<Snapshot>();
    snap->push_back(ownLine(s, display, !clientFocused));
    for (const auto& w : wins) snap->push_back(windowLine(w));
    if (sc.windows.count(0)) {
        const bool paged = sc.pageWindow == 0;
        if (!localElements(s, 0, paged ? sc.pageRoot : 0, paged ? sc.from : 0, paged ? sc.count : 20, *snap))
            return done({}, {}, "no " + std::to_string(sc.pageWindow) + "." + std::to_string(sc.pageRoot) +
                                    " (gone, or never shown)");
    }

    // The apps' views, each from its own endpoint.
    ViewState& st = stateOf(s);
    std::vector<FetchPtr> fetches;
    std::map<int, std::string> keys;
    for (int n : sc.windows) {
        if (n == 0) continue;
        const WinInfo* w = findWin(wins, n);
        if (!w) {
            if (sc.windows.size() == 1) return done({}, {}, "no window w" + std::to_string(n));
            continue;
        }
        const std::string ep = endpointForPid(w->pid);
        if (ep.empty()) continue;
        std::vector<std::string> argv{"view", "--rects"};
        std::string key = "default";
        if (sc.pageWindow == n) {
            argv.push_back("0." + std::to_string(sc.pageRoot));
            argv.push_back("--from=" + std::to_string(sc.from));
            argv.push_back("--count=" + std::to_string(sc.count));
            key = argv[2] + ":" + std::to_string(sc.from) + ":" + std::to_string(sc.count);
        }
        auto cached = st.apps.find(w->id);
        if (cached != st.apps.end() && cached->second.key == key)
            argv.push_back("--if-changed=" + std::to_string(cached->second.rev));
        keys[n] = key;
        fetches.push_back(startFetch(ep, std::move(argv), n));
    }
    if (fetches.empty()) return done(std::move(*snap), std::move(wins), {});

    ControlServer* server = &s;
    whenAll(s, fetches, 2000, [server, fetches, snap, wins, keys, done]() mutable {
        ViewState& st = stateOf(*server);
        std::set<int> answered;
        for (const auto& f : fetches) {
            const WinInfo* w = findWin(wins, f->window);
            if (!w || !f->ok) continue;
            const std::string& p = f->payload;
            const size_t eol = p.find('\n');
            const std::string head = p.substr(0, eol);
            const uint64_t rev = head.rfind("@view ", 0) == 0 ? parseRev(head.substr(6)) : 0;
            AppCache& cache = st.apps[w->id];
            if (!(head.size() > 2 && head.compare(head.size() - 2, 2, " =") == 0 && cache.rev == rev &&
                  cache.key == keys[f->window])) {
                cache.items = view::parseItems(p);
                cache.rev = rev;
                cache.key = keys[f->window];
            }
            placeAppItems(*w, cache.items, *snap);
            answered.insert(f->window);
        }
        for (const auto& f : fetches)
            if (!answered.count(f->window))
                for (Item& it : *snap)
                    if (it.handle == "w" + std::to_string(f->window)) it.extra = it.extra.empty() ? "no view" : it.extra + ", no view";
        done(std::move(*snap), std::move(wins), {});
    });
}

uint64_t parseRev(const std::string& s) {
    if (s.empty()) return 0;
    const char* p = s.c_str();
    if (*p == 'r') ++p;
    return std::strtoull(p, nullptr, 10);
}

// The default scope: this document, and the focused window's elements.
Scope defaultScope(const std::vector<WinInfo>& wins) {
    Scope sc;
    sc.key = "default";
    sc.windows.insert(0);
    for (const auto& w : wins)
        if (w.focused) sc.windows.insert(w.n);
    return sc;
}

// The scope a `view` argument names.
bool scopeFor(ControlServer& s, const ControlCallPtr& call, Scope& sc, std::string& why) {
    const std::string t = call->arg(0);
    if (t.empty()) {
        sc = defaultScope(aggregates(s) ? desktopWindows(s) : std::vector<WinInfo>{});
        return true;
    }
    if (view::isWindowHandle(t)) {
        sc.key = t;
        sc.windows.insert(view::windowOf(t));
        return true;
    }
    if (view::isElementHandle(t)) {
        sc.windows.insert(view::windowOf(t));
        sc.pageWindow = view::windowOf(t);
        sc.pageRoot = view::elementOf(t);
        sc.from = std::max(0, static_cast<int>(call->number("from", 0)));
        sc.count = std::clamp(static_cast<int>(call->number("count", 20)), 1, 400);
        sc.key = t + ":" + std::to_string(sc.from) + ":" + std::to_string(sc.count);
        return true;
    }
    why = "view [wN | N.M] [--since=rN] [--from=K] [--count=20] [--rects]: '" + t + "' is not a handle";
    return false;
}

// Only the items a scope shows (the window lines, and its windows' elements).
Snapshot filterTo(const Snapshot& snap, const Scope& sc) {
    Snapshot out;
    for (const Item& it : snap)
        if (it.isWindow() || sc.windows.count(view::windowOf(it.handle))) out.push_back(it);
    return out;
}

void cmdView(ControlServer& s, const ControlCallPtr& call) {
    Scope sc;
    std::string why;
    if (!scopeFor(s, call, sc, why)) return call->fail(why);
    const uint64_t since = parseRev(call->option("since"));
    const std::string ifChanged = call->option("if-changed");
    const bool rects = call->flag("rects");
    ControlServer* server = &s;
    collectView(s, sc, [server, call, sc, since, ifChanged, rects](Snapshot snap, std::vector<WinInfo>, std::string err) {
        if (!err.empty()) return call->fail(err);
        ViewState& st = stateOf(*server);
        const uint64_t rev = st.history.commit(sc.key, std::move(snap));
        if (!ifChanged.empty()) {
            if (parseRev(ifChanged) == rev) return call->ok(view::noChange(rev));
            return call->ok(st.history.reply(sc.key, 0, rects));
        }
        call->ok(st.history.reply(sc.key, since, rects));
    });
}

void cmdWindows(ControlServer& s, const ControlCallPtr& call) {
    const bool display = aggregates(s);
    auto wins = display ? desktopWindows(s) : std::vector<WinInfo>{};
    bool clientFocused = false;
    for (const auto& w : wins) clientFocused = clientFocused || w.focused;
    std::string out = view::formatItem(ownLine(s, display, !clientFocused), false) + "\n";
    for (const auto& w : wins) out += view::formatItem(windowLine(w), false) + "\n";
    call->ok(out);
}

// ---------------------------------------------------------------------------
// Acting by handle
// ---------------------------------------------------------------------------

using Sink = std::function<void(bool, std::string)>;

// Runs one of the wrapped (original) commands in-process.
void runOriginal(ControlServer& s, const std::string& name, std::vector<std::string> argv, Sink sink) {
    ViewState& st = stateOf(s);
    auto it = st.originals.find(name);
    ControlServer::Handler h = it != st.originals.end() ? it->second : s.handler(name);
    if (!h) return sink(false, "no command '" + name + "'");
    argv.insert(argv.begin(), name);
    h(std::make_shared<ControlCall>(std::move(argv), std::move(sink)));
}

// Runs commands one after another, stopping at the first failure.
void runChain(ControlServer& s, std::shared_ptr<std::vector<std::vector<std::string>>> steps, size_t i, Sink done) {
    if (i >= steps->size()) return done(true, {});
    std::vector<std::string> argv = (*steps)[i];
    const std::string name = argv.front();
    argv.erase(argv.begin());
    ControlServer* server = &s;
    auto next = [server, steps, i, done](bool ok, std::string payload) {
        if (!ok) return done(false, payload);
        runChain(*server, steps, i + 1, done);
    };
    if (name == "eval") return s.dispatch({"eval", argv.empty() ? std::string() : argv[0]}, next);
    runOriginal(s, name, std::move(argv), next);
}

// Until this process and the apps on screen have drawn nothing for a few
// frames (or the timeout): the apps' frames count, not only the shell's.
void settle(ControlServer& s, const std::vector<WinInfo>& wins, double timeoutMs, std::function<void()> then) {
    const std::string t = "--timeout=" + std::to_string(static_cast<int>(timeoutMs));
    std::vector<FetchPtr> fetches;
    for (const auto& w : wins) {
        if (w.minimized) continue;
        const std::string ep = endpointForPid(w.pid);
        if (!ep.empty()) fetches.push_back(startFetch(ep, {"wait-idle", "--frames=3", t}, w.n));
    }
    auto localDone = std::make_shared<bool>(false);
    ControlServer* server = &s;
    runOriginal(s, "wait-idle", {"--frames=3", t}, [localDone](bool, std::string) { *localDone = true; });
    const double start = s.clockMs();
    auto tick = [server, fetches, localDone, start, timeoutMs, then]() {
        bool all = *localDone;
        for (const auto& f : fetches)
            if (!f->done.load(std::memory_order_acquire)) all = false;
        if (!all && server->clockMs() - start < timeoutMs + 500) return false;
        for (const auto& f : fetches)
            if (!f->done.load(std::memory_order_acquire)) cancelFetch(*f);
        then();
        return true;
    };
    s.addTicker(tick);
}

std::string point(float x, float y) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f,%.1f", x, y);
    return b;
}

// The point an action aims at: the visible middle of the item.
void aimAt(const Item& it, const std::vector<WinInfo>& wins, float& x, float& y) {
    float x0 = it.x, y0 = it.y, x1 = it.x + it.w, y1 = it.y + it.h;
    if (!it.isWindow()) {
        if (const WinInfo* w = findWin(wins, view::windowOf(it.handle))) {
            x0 = std::max(x0, w->x);
            y0 = std::max(y0, w->y);
            x1 = std::min(x1, w->x + w->w);
            y1 = std::min(y1, w->y + w->h);
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        x0 = it.x;
        y0 = it.y;
        x1 = it.x + it.w;
        y1 = it.y + it.h;
    }
    x = (x0 + x1) / 2;
    y = (y0 + y1) / 2;
}

// What an action does once its target is known: the commands to run.
using Plan = std::function<bool(const Item* target, const WinInfo* window, std::vector<std::vector<std::string>>& steps,
                                std::string& why)>;

// The shape of every action: the view before, the input, the settle, and
// the diff since the view before as the reply.
void act(ControlServer& s, const ControlCallPtr& call, const std::string& target, Plan plan, double waitWindowMs = 0) {
    const bool display = aggregates(s);
    auto wins0 = display ? desktopWindows(s) : std::vector<WinInfo>{};
    const Scope def = defaultScope(wins0);
    Scope look = def;
    const int tw = target.empty() ? -1 : view::windowOf(target);
    if (tw > 0 && !display) return call->fail("there is only w0 here (no window manager)");
    if (tw > 0) look.windows.insert(tw);
    const uint64_t since = parseRev(call->option("since"));
    const bool rects = call->flag("rects");
    ControlServer* server = &s;
    collectView(s, look, [server, call, def, target, plan, since, rects, waitWindowMs](Snapshot before,
                                                                                      std::vector<WinInfo> wins,
                                                                                      std::string err) {
        if (!err.empty()) return call->fail(err);
        ViewState& st = stateOf(*server);
        const Item* hit = nullptr;
        for (const Item& it : before)
            if (it.handle == target) hit = &it;
        if (!target.empty() && !hit) return call->fail("no " + target + " in view (gone, hidden or scrolled away)");
        const WinInfo* win = target.empty() ? nullptr : findWin(wins, view::windowOf(target));
        auto steps = std::make_shared<std::vector<std::vector<std::string>>>();
        std::string why;
        if (!plan(hit, win, *steps, why)) return call->fail(why);
        const uint64_t rev0 = st.history.commit(def.key, filterTo(before, def));
        const size_t winCount = wins.size();
        runChain(*server, steps, 0, [server, call, since, rects, rev0, wins, winCount, waitWindowMs](bool ok,
                                                                                                   std::string payload) {
            if (!ok) return call->fail(payload);
            auto finish = [server, call, since, rects, rev0]() {
                const bool display = aggregates(*server);
                auto now = display ? desktopWindows(*server) : std::vector<WinInfo>{};
                const Scope after = defaultScope(now);
                collectView(*server, after, [server, call, after, since, rects, rev0](Snapshot snap,
                                                                                    std::vector<WinInfo>,
                                                                                    std::string err) {
                    if (!err.empty()) return call->fail(err);
                    ViewState& st = stateOf(*server);
                    st.history.commit(after.key, std::move(snap));
                    call->ok(st.history.reply(after.key, since ? since : rev0, rects));
                });
            };
            auto doSettle = [server, finish]() {
                auto now = aggregates(*server) ? desktopWindows(*server) : std::vector<WinInfo>{};
                settle(*server, now, 2000, finish);
            };
            if (waitWindowMs <= 0) return doSettle();
            // launch: until a new window maps (or the wait runs out).
            const double start = server->clockMs();
            server->addTicker([server, winCount, start, waitWindowMs, doSettle]() {
                if (desktopWindows(*server).size() <= winCount && server->clockMs() - start < waitWindowMs)
                    return false;
                doSettle();
                return true;
            });
        });
    });
}

// Whether an input command's first argument is a handle (its agent form).
bool handleForm(const ControlCallPtr& call) {
    auto p = call->positional();
    if (p.empty()) return false;
    if (view::isWindowHandle(p[0])) return true;
    if (!view::isElementHandle(p[0])) return false;
    // `type 1.5` types "1.5"; `type 3.1 text` types into 3.1.
    if (call->argv()[0] == "type") return p.size() > 1;
    // "3.4 5": a point, not a handle.
    if (p.size() > 1) {
        char* end = nullptr;
        std::strtof(p[1].c_str(), &end);
        if (end && *end == '\0' && !p[1].empty()) return false;
    }
    return true;
}

std::string windowId(const WinInfo* w) { return w ? std::to_string(w->id) : "0"; }

std::vector<std::string> focusStep(const WinInfo* w) {
    return {"eval", "bro.compositor.focusWindow(" + windowId(w) + "), true"};
}

// Options an input command passes on (--button, --count, ...).
std::vector<std::string> passOptions(const ControlCallPtr& call, std::initializer_list<const char*> names) {
    std::vector<std::string> out;
    for (const char* n : names) {
        const std::string v = call->option(n);
        if (!v.empty()) out.push_back(std::string("--") + n + "=" + v);
    }
    return out;
}

std::string restText(const ControlCallPtr& call, size_t from) {
    auto p = call->positional();
    std::string t;
    for (size_t i = from; i < p.size(); ++i) t += (i > from ? " " : "") + p[i];
    return t;
}

void cmdClickHandle(ControlServer& s, const ControlCallPtr& call) {
    const std::string target = call->arg(0);
    act(s, call, target, [call](const Item* it, const WinInfo* w, auto& steps, std::string&) {
        if (it->isWindow()) {
            if (w) steps.push_back(focusStep(w));
            return true;
        }
        float x, y;
        aimAt(*it, w ? std::vector<WinInfo>{*w} : std::vector<WinInfo>{}, x, y);
        std::vector<std::string> c{"click", point(x, y)};
        for (auto& o : passOptions(call, {"button", "count", "hold"})) c.push_back(o);
        steps.push_back(std::move(c));
        return true;
    });
}

// Steps that give `it` the keyboard: its window focused, and an element not
// focused yet clicked. (A focused field is not clicked again: a second click
// there would read as a double click.)
void focusSteps(const Item* it, const WinInfo* w, std::vector<std::vector<std::string>>& steps) {
    if (it->isWindow()) {
        if (w) steps.push_back(focusStep(w));
    } else if (it->flags.find('@') != std::string::npos) {
        if (w && !w->focused) steps.push_back(focusStep(w));
    } else {
        float x, y;
        aimAt(*it, w ? std::vector<WinInfo>{*w} : std::vector<WinInfo>{}, x, y);
        steps.push_back({"click", point(x, y)});
    }
}

void cmdTypeHandle(ControlServer& s, const ControlCallPtr& call, bool replace) {
    const std::string target = call->arg(0);
    const std::string text = restText(call, 1);
    act(s, call, target, [text, replace](const Item* it, const WinInfo* w, auto& steps, std::string&) {
        focusSteps(it, w, steps);
        const bool field = it->role == "textbox" || it->role == "searchbox" || it->role == "spinbutton";
        if (replace) {
            steps.push_back({"key", "ctrl+a"});
            if (text.empty()) steps.push_back({"key", "backspace"});
        } else if (field) {
            steps.push_back({"key", "ctrl+end"});  // type appends
        }
        if (!text.empty()) steps.push_back({"type", text});
        return true;
    });
}

void cmdKeyHandle(ControlServer& s, const ControlCallPtr& call) {
    auto p = call->positional();
    const bool toTarget = !p.empty() && (view::isWindowHandle(p[0]) || view::isElementHandle(p[0]));
    const std::string target = toTarget ? p[0] : std::string();
    std::vector<std::string> keys(p.begin() + (toTarget ? 1 : 0), p.end());
    if (keys.empty()) return call->fail("usage: key [wN | N.M] <combo>...");
    auto opts = passOptions(call, {"hold", "gap"});
    act(s, call, target, [keys, opts](const Item* it, const WinInfo* w, auto& steps, std::string&) {
        if (it) focusSteps(it, w, steps);
        std::vector<std::string> k{"key"};
        k.insert(k.end(), keys.begin(), keys.end());
        k.insert(k.end(), opts.begin(), opts.end());
        steps.push_back(std::move(k));
        return true;
    });
}

void cmdScroll(ControlServer& s, const ControlCallPtr& call) {
    const std::string target = call->arg(0);
    const std::string dy = call->arg(1, "3");
    if (!view::isElementHandle(target) && !view::isWindowHandle(target))
        return call->fail("usage: scroll <N.M | wN> [detents=3, + is down]");
    act(s, call, target, [dy](const Item* it, const WinInfo* w, auto& steps, std::string&) {
        float x, y;
        aimAt(*it, w ? std::vector<WinInfo>{*w} : std::vector<WinInfo>{}, x, y);
        steps.push_back({"wheel", dy, "--at=" + point(x, y)});
        return true;
    });
}

void cmdFocus(ControlServer& s, const ControlCallPtr& call, bool close) {
    const std::string target = call->arg(0);
    if (!view::isWindowHandle(target)) return call->fail(std::string("usage: ") + (close ? "close" : "focus") + " wN");
    if (view::windowOf(target) == 0) {
        if (close) return call->fail("w0 is this process; it is not closed from here");
        return act(s, call, {}, [](const Item*, const WinInfo*, auto&, std::string&) { return true; });
    }
    act(s, call, target, [close](const Item*, const WinInfo* w, auto& steps, std::string& why) {
        if (!w) return why = "no such window", false;
        steps.push_back({"eval", std::string("bro.compositor.") + (close ? "closeWindow(" : "focusWindow(") +
                                     windowId(w) + "), true"});
        return true;
    });
}

std::string jsString(const std::string& s) { return view::quote(s); }

void cmdLaunch(ControlServer& s, const ControlCallPtr& call) {
    const std::string id = call->arg(0);
    if (id.empty()) return call->fail("usage: launch <app id>  (a desktop entry id: helm.calculator)");
    // bro.apps resolves the id (with or without .desktop); a token lets its
    // first window take focus.
    const std::string js =
        "(async () => { const id = " + jsString(id) +
        "; const c = bro.compositor; const t = c && c.activationToken ? c.activationToken(id) : null;"
        " const o = t ? { env: { XDG_ACTIVATION_TOKEN: t, DESKTOP_STARTUP_ID: t } } : undefined;"
        " const ids = id.endsWith('.desktop') ? [id] : [id, id + '.desktop']; let last;"
        " for (const x of ids) { try { await bro.apps.launch(x, o); return true; } catch (e) { last = e; } }"
        " throw last; })()";
    act(
        s, call, {},
        [js](const Item*, const WinInfo*, auto& steps, std::string&) {
            steps.push_back({"eval", js});
            return true;
        },
        aggregates(s) ? 8000 : 0);
}

// ---------------------------------------------------------------------------
// shot / who-has-keys / clipboard
// ---------------------------------------------------------------------------

void cmdShot(ControlServer& s, const ControlCallPtr& call) {
    std::string target = call->arg(0);
    std::string path = call->arg(1);
    if (!target.empty() && !view::isWindowHandle(target) && !view::isElementHandle(target)) {
        path = target;  // shot <path>: the screen
        target.clear();
    }
    if (path.empty()) path = controlRuntimePath("shot.png");
    const std::string scale = call->option("scale", "1");
    if (target.empty()) return runOriginal(s, "screenshot", {path, "--scale=" + scale}, [call](bool ok, std::string p) {
        ok ? call->ok(p) : call->fail(p);
    });
    if (!view::isWindowHandle(target) && !view::isElementHandle(target))
        return call->fail("usage: shot [N.M | wN] [path] [--scale=F]");
    const int n = view::windowOf(target);
    ControlServer* server = &s;
    auto sink = [call](bool ok, std::string p) { ok ? call->ok(p) : call->fail(p); };
    if (n == 0) {
        if (view::isWindowHandle(target)) return runOriginal(s, "screenshot", {path, "--scale=" + scale}, sink);
        Scope sc;
        sc.windows.insert(0);
        return collectView(s, sc, [server, call, target, path, scale, sink](Snapshot snap, std::vector<WinInfo>,
                                                                            std::string err) {
            if (!err.empty()) return call->fail(err);
            for (const Item& it : snap)
                if (it.handle == target) {
                    char region[96];
                    std::snprintf(region, sizeof(region), "--region=%.0f,%.0f,%.0f,%.0f", it.x, it.y, it.w, it.h);
                    return runOriginal(*server, "screenshot", {path, region, "--scale=" + scale}, sink);
                }
            call->fail("no " + target + " in view");
        });
    }
    // An app's window: the app's own picture (its window even when covered).
    auto wins = aggregates(s) ? desktopWindows(s) : std::vector<WinInfo>{};
    const WinInfo* w = findWin(wins, n);
    if (!w) return call->fail("no window w" + std::to_string(n));
    const std::string ep = endpointForPid(w->pid);
    if (ep.empty()) return call->fail("w" + std::to_string(n) + " serves no control endpoint (not a bro app?)");
    std::vector<std::string> argv{"screenshot", path, "--scale=" + scale};
    if (view::isElementHandle(target)) {
        // The element's rect in the app's own coordinates, from its last view.
        ViewState& st = stateOf(s);
        auto c = st.apps.find(w->id);
        const Item* hit = nullptr;
        if (c != st.apps.end())
            for (const Item& it : c->second.items)
                if (view::elementOf(it.handle) == view::elementOf(target)) hit = &it;
        if (!hit) return call->fail("no " + target + " in the last view of w" + std::to_string(n) + " (view it first)");
        char region[96];
        std::snprintf(region, sizeof(region), "--region=%.0f,%.0f,%.0f,%.0f", hit->x, hit->y, hit->w, hit->h);
        argv.push_back(region);
    }
    auto f = startFetch(ep, std::move(argv), n);
    whenAll(s, {f}, 5000, [f, call]() { f->ok ? call->ok(f->payload) : call->fail(f->payload); });
}

void cmdWhoHasKeys(ControlServer& s, const ControlCallPtr& call) {
    auto wins = aggregates(s) ? desktopWindows(s) : std::vector<WinInfo>{};
    collectView(s, defaultScope(wins), [call](Snapshot snap, std::vector<WinInfo>, std::string err) {
        if (!err.empty()) return call->fail(err);
        std::string out;
        for (const Item& it : snap)
            if ((it.isWindow() && it.flags.find('*') != std::string::npos) ||
                (!it.isWindow() && it.flags.find('@') != std::string::npos))
                out += view::formatItem(it, false) + "\n";
        call->ok(out);
    });
}

void cmdClipboard(ControlServer& s, const ControlCallPtr& call) {
    auto local = [&s, call]() {
        s.dispatch({"eval", "navigator.clipboard.readText()"}, [call](bool ok, std::string p) {
            ok ? call->ok(p) : call->fail(p);
        });
    };
    // A display's own clipboard is not its clients' selection (the
    // compositor hands that only to the focused client): ask the focused
    // app, which reads it as any app would.
    if (!aggregates(s) || call->flag("local")) return local();
    std::string ep;
    for (const auto& w : desktopWindows(s))
        if (w.focused) ep = endpointForPid(w.pid);
    if (ep.empty()) return local();
    auto f = startFetch(ep, {"clipboard", "--local"}, 0);
    whenAll(s, {f}, 2000, [f, call]() { f->ok ? call->ok(f->payload) : call->fail(f->payload); });
}

}  // namespace

bool controlDisplayServes() {
    for (const auto& n : brolink::list_local(control::kApp))
        if (n == "display") return true;
    return false;
}

void registerAgentViewCommands(ControlServer& s) {
    ViewState& st = stateOf(s);
    // The input commands' own forms stay; a handle (or --diff) takes the agent path.
    for (const char* name : {"click", "type", "key", "screenshot", "wait-idle", "wheel"}) {
        std::string usage;
        if (auto h = s.handler(name, &usage)) st.originals[name] = h;
    }
    auto wrap = [&s, &st](const char* name, std::string usage, std::function<void(const ControlCallPtr&)> agent) {
        auto orig = st.originals[name];
        s.registerCommand(name, std::move(usage), [orig, agent](const ControlCallPtr& c) {
            if (handleForm(c) || c->flag("diff")) return agent(c);
            if (orig) orig(c);
        });
    };
    // The plain form with --diff: the same command, answered with the diff.
    auto plainWithDiff = [&s](const ControlCallPtr& c) {
        std::vector<std::string> argv;
        for (size_t i = 0; i < c->argv().size(); ++i)
            if (c->argv()[i] != "--diff") argv.push_back(c->argv()[i]);
        act(s, c, {}, [argv](const Item*, const WinInfo*, auto& steps, std::string&) {
            steps.push_back(argv);
            return true;
        });
    };
    std::string usage;
    s.handler("click", &usage);
    wrap("click", "<N.M | wN> | " + usage + "  (a handle: the reply is the view's diff)",
         [&s, plainWithDiff](const ControlCallPtr& c) { handleForm(c) ? cmdClickHandle(s, c) : plainWithDiff(c); });
    s.handler("type", &usage);
    wrap("type", "<N.M> <text> | " + usage + "  (a handle: focus it, type, reply with the diff)",
         [&s, plainWithDiff](const ControlCallPtr& c) {
             handleForm(c) ? cmdTypeHandle(s, c, false) : plainWithDiff(c);
         });
    s.handler("key", &usage);
    wrap("key", "[wN | N.M] <combo>... | " + usage +
                    "  (wN / N.M: give it the keyboard first; with a handle or --diff the reply is the diff)",
         [&s](const ControlCallPtr& c) { cmdKeyHandle(s, c); });

    s.registerCommand("view",
                      "[wN | N.M] [--since=rN] [--from=K] [--count=20] [--rects]  the agent view: windows, and the "
                      "focused window's elements with handles; --since: the diff since a revision; N.M: one list, "
                      "paged",
                      [&s](const ControlCallPtr& c) { cmdView(s, c); });
    s.registerCommand("windows", " the window lines: wN, focus (*), app id, title, rect; top first",
                      [&s](const ControlCallPtr& c) { cmdWindows(s, c); });
    s.registerCommand("set", "<N.M> <text>  replace a field's text (focus, select all, type); replies with the diff",
                      [&s](const ControlCallPtr& c) { cmdTypeHandle(s, c, true); });
    s.registerCommand("scroll", "<N.M | wN> [detents=3]  wheel over it (+ is down); replies with the diff",
                      [&s](const ControlCallPtr& c) { cmdScroll(s, c); });
    s.registerCommand("focus", "<wN>  focus a window; replies with the diff",
                      [&s](const ControlCallPtr& c) { cmdFocus(s, c, false); });
    s.registerCommand("close", "<wN>  close a window; replies with the diff",
                      [&s](const ControlCallPtr& c) { cmdFocus(s, c, true); });
    s.registerCommand("launch", "<app id>  start an app (bro.apps), wait for its window; replies with the diff",
                      [&s](const ControlCallPtr& c) { cmdLaunch(s, c); });
    s.registerCommand("shot", "[N.M | wN] [path] [--scale=1]  PNG of an element, a window (its own buffer, even "
                              "covered), or the screen",
                      [&s](const ControlCallPtr& c) { cmdShot(s, c); });
    s.registerCommand("who-has-keys", " the focused window and the element with keyboard focus",
                      [&s](const ControlCallPtr& c) { cmdWhoHasKeys(s, c); });
    s.registerCommand("clipboard", "[--local]  the clipboard's text, as the apps see it (a display asks the focused app)",
                      [&s](const ControlCallPtr& c) { cmdClipboard(s, c); });
}

}  // namespace bro::engine

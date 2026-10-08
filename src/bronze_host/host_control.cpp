// The agent-control commands that need the page's JS realm
// (docs/agent-control.md): eval, and the DOM views the headless inspector
// already draws (inspect, dom, style). Registered on the engine's
// ControlServer (engine/control.h) when the web host globals install; they
// run on the engine thread between frames, like every control command.
//
// eval compiles the text in-process (bronze's JIT, as `eval()` does) and
// replies with its completion value: a string as itself, anything else as
// JSON (elements as "<tag#id.class>"). A promise is awaited — `(async () =>
// { ... })()` is how a script waits — and a throw or rejection replies as an
// error carrying the stack.
//
// Headless tests reach the same commands through controlCommand(...argv),
// which returns an id, and controlResult(id): null while the command is
// still running (advanceTime plays it out), else { ok, payload }.

#include "bronze_host/host_control.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_headless_internal.h"

#include "dom/document.h"
#include "dom/element.h"
#include "engine/control.h"
#include "engine/engine.h"
#include "util/json_out.h"
#include "util/time.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>

namespace bro::bronze_host {

namespace {

constexpr const char* kSettleSource = R"JS((function (id, v, thrown, reply) {
  function fmt(x) {
    if (typeof x === 'string') return x;
    if (x === undefined) return 'undefined';
    if (typeof x === 'function') return String(x);
    var seen = new Set();
    try {
      var s = JSON.stringify(x, function (k, val) {
        if (val && typeof val === 'object') {
          if (typeof val.tagName === 'string' && typeof val.getBoundingClientRect === 'function') {
            var d = '<' + val.tagName.toLowerCase();
            if (val.id) d += '#' + val.id;
            if (typeof val.className === 'string' && val.className.trim())
              d += '.' + val.className.trim().split(/\s+/).join('.');
            return d + '>';
          }
          if (seen.has(val)) return '[circular]';
          seen.add(val);
        }
        if (typeof val === 'bigint') return String(val);
        return val;
      }, 2);
      return s === undefined ? String(x) : s;
    } catch (e) {
      return String(x);
    }
  }
  function err(e) {
    if (e && e.stack) return String(e.stack);
    if (e && e.name) return e.name + ': ' + e.message;
    return String(e);
  }
  if (thrown) { reply(id, false, err(v)); return; }
  if (v && typeof v === 'object' && typeof v.then === 'function') {
    v.then(function (x) { reply(id, true, fmt(x)); }, function (e) { reply(id, false, err(e)); });
    return;
  }
  reply(id, true, fmt(v));
}))JS";

struct EvalState {
    ev::Persistent* settle = nullptr;  // never freed: lives as long as the realm
    ev::Persistent* reply = nullptr;
    uint64_t nextId = 1;
    std::map<uint64_t, engine::ControlCallPtr> pending;
};
EvalState g_eval;

// For controlCommand / controlResult.
struct LocalResult {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string payload;
};
std::map<uint64_t, std::shared_ptr<LocalResult>> g_local;
uint64_t g_localNext = 1;

bool ensureSettle(engine::Engine& engine, std::string* why) {
    if (g_eval.settle) return true;
    auto res = evalScriptJitResult(engine, kSettleSource, "<bro-ctl>");
    if (res.thrown || !ev::isFunction(res.value)) {
        if (why) *why = "could not compile the eval helper";
        return false;
    }
    g_eval.settle = new ev::Persistent(res.value);
    g_eval.reply = new ev::Persistent(ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::undefined();
            const uint64_t id = static_cast<uint64_t>(ev::toDouble(a[0]));
            const bool ok = ev::toBool(a[1]);
            std::string text = ev::toUtf8(a[2]);
            auto it = g_eval.pending.find(id);
            if (it == g_eval.pending.end()) return ev::undefined();
            engine::ControlCallPtr call = it->second;
            g_eval.pending.erase(it);
            if (ok) call->ok(std::move(text));
            else call->fail(std::move(text));
            return ev::undefined();
        },
        3, "reply"));
    return true;
}

void cmdEval(engine::ControlServer& server, const engine::ControlCallPtr& call) {
    engine::Engine& engine = server.engine();
    std::string code;
    for (const auto& a : call->positional()) code += (code.empty() ? "" : " ") + a;
    if (code.empty()) return call->fail("usage: eval <javascript>  (a promise is awaited)");
    std::string why;
    if (!ensureSettle(engine, &why)) return call->fail(why);

    const uint64_t id = g_eval.nextId++;
    g_eval.pending[id] = call;
    auto res = evalScriptJitResult(engine, code, "<bro-ctl eval>");
    ev::Persistent value(res.value);
    ev::Persistent idv(ev::fromDouble(static_cast<double>(id)));
    ev::Persistent thrown(ev::fromBool(res.thrown));
    Value args[4] = {idv.get(), value.get(), thrown.get(), g_eval.reply->get()};
    auto r = ev::call(g_eval.settle->get(), ev::undefined(), std::span<const Value>(args, 4));
    if (r.thrown && !call->done()) {
        g_eval.pending.erase(id);
        call->fail("formatting the result threw: " + ev::toUtf8(r.value));
        return;
    }
    if (call->done()) return;

    // A promise: settled by a later frame's microtasks, or timed out.
    engine::ControlServer* s = &server;
    const double deadline = server.clockMs() + call->number("timeout", 10000);
    server.addTicker([s, call, id, deadline]() {
        if (call->done()) return true;
        if (s->clockMs() < deadline) return false;
        g_eval.pending.erase(id);
        call->fail("the promise did not settle in time (--timeout=ms)");
        return true;
    });
}

dom::Element* queryOne(engine::Engine& engine, const std::string& sel, std::string* why) {
    dom::Document* doc = engine.document();
    if (!doc) {
        if (why) *why = "no document";
        return nullptr;
    }
    dom::Element* el = nullptr;
    try {
        el = doc->querySelector(sel);
    } catch (...) {
    }
    if (!el && why) *why = "nothing matches '" + sel + "'";
    if (el) engine.flushLayoutForRead(doc);
    return el;
}

void cmdDom(engine::ControlServer& server, const engine::ControlCallPtr& call) {
    std::string why;
    dom::Element* el = queryOne(server.engine(), call->arg(0, "body"), &why);
    if (!el) return call->fail(why);
    const int depth = std::atoi(call->arg(1, "4").c_str());
    std::ostringstream out;
    buildTreeString(out, el, 0, depth, "");
    call->ok(out.str());
}

void cmdInspect(engine::ControlServer& server, const engine::ControlCallPtr& call) {
    std::string why;
    dom::Element* el = queryOne(server.engine(), call->arg(0), &why);
    if (!el) return call->fail(call->arg(0).empty() ? "usage: inspect <selector> [--verbose]" : why);
    call->ok(buildInspectString(el, call->flag("verbose")));
}

void cmdStyle(engine::ControlServer& server, const engine::ControlCallPtr& call) {
    auto args = call->positional();
    if (args.empty()) return call->fail("usage: style <selector> [property...]");
    std::string why;
    dom::Element* el = queryOne(server.engine(), args[0], &why);
    if (!el) return call->fail(why);
    const auto& cs = el->computedStyle();
    util::JsonOut j;
    j.beginObject();
    if (args.size() > 1) {
        for (size_t i = 1; i < args.size(); ++i) {
            auto it = cs.find(args[i]);
            j.key(args[i]);
            if (it != cs.end()) j.string(it->second);
            else j.null();
        }
    } else {
        for (const auto& [k, v] : cs) j.key(k).string(v);
    }
    j.endObject();
    call->ok(j.take());
}

}  // namespace

void installControlCommands(engine::Engine& engine) {
    engine::ControlServer& s = engine.control();
    s.registerCommand("eval",
                      "<javascript> [--timeout=10000]  run in the page's realm; prints the completion value "
                      "(JSON unless a string); a promise is awaited",
                      [&s](const engine::ControlCallPtr& c) { cmdEval(s, c); });
    s.registerCommand("dom", "[selector=body] [depth=4]  the layout tree under an element: tag, size, position",
                      [&s](const engine::ControlCallPtr& c) { cmdDom(s, c); });
    s.registerCommand("inspect", "<selector> [--verbose]  box model, position, computed styles of the first match",
                      [&s](const engine::ControlCallPtr& c) { cmdInspect(s, c); });
    s.registerCommand("style", "<selector> [property...]  computed style of the first match (JSON)",
                      [&s](const engine::ControlCallPtr& c) { cmdStyle(s, c); });

    // Headless tests: drive the same commands without a socket.
    ev::registerGlobal("controlCommand", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            std::vector<std::string> argv;
            for (const Value& v : a) argv.push_back(ev::toUtf8(v));
            const uint64_t id = g_localNext++;
            auto res = std::make_shared<LocalResult>();
            g_local[id] = res;
            engine.control().dispatch(std::move(argv), [res](bool ok, std::string payload) {
                res->ok = ok;
                res->payload = std::move(payload);
                // Last, and released: a screenshot replies from its encode thread.
                res->done.store(true, std::memory_order_release);
            });
            return ev::fromDouble(static_cast<double>(id));
        },
        1, "controlCommand"));
    ev::registerGlobal("controlResult", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::null();
            auto it = g_local.find(static_cast<uint64_t>(ev::toDouble(a[0])));
            if (it == g_local.end() || !it->second->done.load(std::memory_order_acquire)) return ev::null();
            auto res = it->second;
            g_local.erase(it);
            ev::Persistent obj(ev::createObject());
            ev::Persistent okv(ev::fromBool(res->ok));
            obj.set(ev::setProperty(obj.get(), "ok", okv.get()));
            ev::Persistent pv(ev::fromUtf8(res->payload));
            obj.set(ev::setProperty(obj.get(), "payload", pv.get()));
            return obj.get();
        },
        1, "controlResult"));
}

}  // namespace bro::bronze_host

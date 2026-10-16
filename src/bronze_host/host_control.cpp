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
#include "platform/control_protocol.h"
#include "platform/inbox_bundle.h"
#include "util/json_out.h"
#include "util/log.h"
#include "util/main_loop_wake.h"
#include "util/time.h"

#include <brolink/paths.h>
#include <brolink/stream.h>
#include <brolink/wire.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

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

// ---------------------------------------------------------------------------
// bro.inbox (docs/inbox-api.js, docs/agent-inbox.md): the agent inbox for a
// page, over a small control-protocol client. A command goes to the session's
// inbox: in-process when this engine is the display (helm's own UI) or no
// display serves (a lone window, headless), otherwise to the display's control
// endpoint on a thread of its own (an app of the session). Replies are handed
// to their callbacks by a frame pump, never inside the call that asked.
// ---------------------------------------------------------------------------

struct ClientDone {
    uint64_t id = 0;
    bool ok = false;
    std::string payload;
    std::string file;
};
std::mutex g_clientMu;
std::vector<ClientDone> g_clientDone;          // under g_clientMu
std::map<uint64_t, ev::Persistent*> g_clientCb;  // engine thread
uint64_t g_clientNext = 1;
ev::Persistent* g_inboxNs = nullptr;  // the compiled namespace, once read

void clientFinished(uint64_t id, bool ok, std::string payload, std::string file) {
    {
        std::lock_guard<std::mutex> lk(g_clientMu);
        g_clientDone.push_back({id, ok, std::move(payload), std::move(file)});
    }
    util::wakeMainLoop();
}

// The address to send to, or empty for in-process. `endpoint`: "" the
// session's inbox, "self" this engine's, else a control endpoint's name.
std::string clientAddress(engine::Engine& engine, const std::string& endpoint) {
    if (endpoint == "self") return {};
    const std::string name = endpoint.empty() ? std::string("display") : endpoint;
    const std::string address = brolink::local_address(platform::control::kApp, name);
    if (endpoint.empty()) {
        // A headless engine is no part of the session (a test beside a live
        // desktop must not reach it): its own inbox, unless one is named.
        if (engine.displayMode() == engine::DisplayMode::Headless) return {};
        if (!engine::controlDisplayServes()) return {};
        if (engine.control().running() && engine.control().socketPath() == address) return {};
    }
    return address;
}

// One request on a connection of its own, on a thread of its own (a held
// poll can take its time without holding anything else up).
void clientSend(uint64_t id, std::string address, std::vector<std::string> argv, uint64_t outArg) {
    std::thread([id, address = std::move(address), argv = std::move(argv), outArg] {
        namespace control = platform::control;
        std::string err;
        auto stream = brolink::connect_local(address, &err);
        if (!stream) return clientFinished(id, false, "cannot reach the inbox at " + address + ": " + err, {});
        if (!stream->write(control::encodeRequest(1, argv, outArg)))
            return clientFinished(id, false, "the inbox connection failed", {});
        brolink::wire::MessageSplitter split(control::kMaxMessage);
        brolink::wire::MessageSplitter::Message m;
        char buf[65536];
        for (;;) {
            while (split.next(m)) {
                uint64_t rid = 0;
                bool ok = false;
                std::string payload, file;
                if (m.type != control::kReply || !control::decodeReply(m.payload, rid, ok, payload, file))
                    return clientFinished(id, false, "a malformed reply from the inbox", {});
                return clientFinished(id, ok, std::move(payload), std::move(file));
            }
            const size_t n = stream->read(buf, sizeof buf);
            if (n == 0) return clientFinished(id, false, "the inbox closed the connection", {});
            split.feed(buf, n);
        }
    }).detach();
}

// The bytes of a JS value: a string's UTF-8, a typed array's or an
// ArrayBuffer's contents.
std::string bytesOf(Value v) {
    if (ev::isString(v)) return ev::toUtf8(v);
    if (ev::TypedArrayInfo t = ev::typedArrayInfo(v)) return std::string(reinterpret_cast<const char*>(t.data), t.byteLength);
    if (ev::ArrayBufferInfo b = ev::arrayBufferInfo(v)) return std::string(reinterpret_cast<const char*>(b.data), b.byteLength);
    return {};
}

uint32_t lengthOf(Value arr) { return static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr, "length"))); }

// `--files=` plus the bundle of [{name, data}].
std::string filesArgument(Value list) {
    ev::Persistent files(list);
    std::vector<platform::inbox::File> out;
    const uint32_t n = lengthOf(files.get());
    for (uint32_t i = 0; i < n; ++i) {
        ev::Persistent f(ev::getElement(files.get(), i));
        platform::inbox::File file;
        file.name = ev::toUtf8(ev::getProperty(f.get(), "name"));
        ev::Persistent data(ev::getProperty(f.get(), "data"));
        file.bytes = bytesOf(data.get());
        out.push_back(std::move(file));
    }
    return "--files=" + platform::inbox::encode(out);
}

// __broControl.call(argv, endpoint, outArg, cb): argv's strings, an element
// {files: [{name, data}]} becoming a --files= bundle; cb(ok, payload, files)
// with files [{name, data: Uint8Array}] from the reply's bundle.
Value nativeControlCall(engine::Engine& engine, std::span<const Value> a) {
    if (a.size() < 4 || !ev::isFunction(a[3])) return ev::undefined();
    ev::Persistent arr(a[0]);
    ev::Persistent cb(a[3]);
    const std::string endpoint = ev::isString(a[1]) ? ev::toUtf8(a[1]) : std::string();
    const uint64_t outArg = static_cast<uint64_t>(ev::toDouble(a[2]));
    std::vector<std::string> argv;
    const uint32_t n = lengthOf(arr.get());
    for (uint32_t i = 0; i < n; ++i) {
        ev::Persistent el(ev::getElement(arr.get(), i));
        if (ev::isString(el.get())) {
            argv.push_back(ev::toUtf8(el.get()));
        } else if (ev::isObject(el.get())) {
            ev::Persistent files(ev::getProperty(el.get(), "files"));
            argv.push_back(filesArgument(files.get()));
        } else {
            argv.push_back(ev::toUtf8(el.get()));
        }
    }
    const uint64_t id = g_clientNext++;
    g_clientCb[id] = new ev::Persistent(cb.get());
    const std::string address = clientAddress(engine, endpoint);
    if (address.empty()) {
        engine.control().dispatchWithFile(
            std::move(argv), static_cast<size_t>(outArg),
            [id](bool ok, std::string payload, std::string file) {
                clientFinished(id, ok, std::move(payload), std::move(file));
            },
            engine::ControlServer::inProcess());
    } else {
        clientSend(id, address, std::move(argv), outArg);
    }
    return ev::fromDouble(static_cast<double>(id));
}

// The frame pump: each finished request's callback, on the engine thread.
void pumpControlClient() {
    std::vector<ClientDone> done;
    {
        std::lock_guard<std::mutex> lk(g_clientMu);
        if (g_clientDone.empty()) return;
        done.swap(g_clientDone);
    }
    for (ClientDone& d : done) {
        auto it = g_clientCb.find(d.id);
        if (it == g_clientCb.end()) continue;
        ev::Persistent* cb = it->second;
        g_clientCb.erase(it);
        std::vector<platform::inbox::File> files;
        std::string why;
        if (d.ok && !platform::inbox::decode(d.file, files, &why)) LOG_WARN("bro.inbox: %s", why.c_str());
        ev::Persistent list(ev::makeArray(0));
        for (uint32_t i = 0; i < files.size(); ++i) {
            ev::Persistent item(ev::createObject());
            ev::Persistent name(ev::fromUtf8(files[i].name));
            ev::setProperty(item.get(), "name", name.get());
            ev::Persistent data(ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(files[i].bytes.size())));
            ev::fillTypedArray(data.get(), std::span<const uint8_t>(
                                               reinterpret_cast<const uint8_t*>(files[i].bytes.data()), files[i].bytes.size()));
            ev::setProperty(item.get(), "data", data.get());
            ev::setElement(list.get(), i, item.get());
        }
        ev::Persistent okv(ev::fromBool(d.ok));
        ev::Persistent payload(ev::fromUtf8(d.payload));
        Value args[3] = {okv.get(), payload.get(), list.get()};
        auto r = ev::call(cb->get(), ev::undefined(), std::span<const Value>(args, 3));
        if (r.thrown) LOG_WARN("bro.inbox: a callback threw: %s", ev::toUtf8(r.value).c_str());
        delete cb;
    }
}

// The namespace itself, in JS over __broControl.call: compiled the first time
// bro.inbox is read, so a page that never touches it pays nothing.
constexpr const char* kInboxSource = R"JS((function (native) {
  'use strict';
  var state = { endpoint: '' };
  function raw(argv, outArg) {
    return new Promise(function (resolve, reject) {
      native.call(argv, state.endpoint, outArg || 0, function (ok, payload, files) {
        if (ok) resolve({ payload: payload, files: files || [] });
        else reject(new Error(payload));
      });
    });
  }
  function json(r) { return JSON.parse(r.payload); }
  function withFiles(r) {
    var s = JSON.parse(r.payload);
    if (s.state === 'done') s.files = r.files;
    return s;
  }
  function me() {
    try { if (globalThis.bro && bro.app && bro.app.id) return String(bro.app.id); } catch (e) {}
    return 'app';
  }
  function filesArg(files) { return files && files.length ? [{ files: files }] : []; }
  function pause(ms) { return new Promise(function (res) { setTimeout(res, ms); }); }
  var ns = {
    get endpoint() { return state.endpoint; },
    set endpoint(v) { state.endpoint = v ? String(v) : ''; },
    agents: function () { return raw(['agents', '--json']).then(json); },
    ask: function (name, text, o) {
      o = o || {};
      var argv = ['ask', String(name), 'files.bin', '--text=' + String(text), '--from=' + (o.from || me())]
        .concat(filesArg(o.files));
      if (o.wait) argv.push('--wait=' + Number(o.wait));
      return raw(argv, 2).then(withFiles);
    },
    status: function (id, o) {
      o = o || {};
      var argv = ['inbox', 'status', String(id), '--since=' + (o.since || 0)];
      if (o.wait) argv.push('--wait=' + Number(o.wait));
      return raw(argv).then(json);
    },
    result: function (id, o) {
      o = o || {};
      var argv = ['inbox', 'result', String(id), 'files.bin'];
      if (o.wait) argv.push('--wait=' + Number(o.wait));
      return raw(argv, 3).then(withFiles);
    },
    cancel: function (id) { return raw(['inbox', 'cancel', String(id), '--from=' + me()]).then(json); },
    log: function (o) {
      o = o || {};
      var argv = ['inbox', 'log', '--limit=' + (o.limit || 50)];
      if (o.id) argv.push('--id=' + o.id);
      return raw(argv).then(function (r) {
        return r.payload.split('\n').filter(function (l) { return l.length > 0; })
          .map(function (l) { return JSON.parse(l); });
      });
    },
    events: function (o) {
      o = o || {};
      var argv = ['inbox', 'events', '--since=' + (o.since || 0)];
      if (o.wait) argv.push('--wait=' + Number(o.wait));
      return raw(argv).then(json);
    },
    watch: function (fn, o) {
      o = o || {};
      var stopped = false;
      var since = o.since;
      (async function () {
        if (since === undefined) {
          try { since = (await ns.events({ since: 0 })).next; } catch (e) { since = 0; }
        }
        while (!stopped) {
          try {
            var r = await ns.events({ since: since, wait: 20 });
            since = r.next;
            for (var i = 0; i < r.events.length && !stopped; i++) {
              try { fn(r.events[i]); } catch (e) { console.error('bro.inbox.watch: ' + e); }
            }
          } catch (e) {
            if (!stopped) await pause(1000);
          }
        }
      })();
      return { stop: function () { stopped = true; } };
    },
    serve: async function (name, o, handler) {
      if (typeof o === 'function') { handler = o; o = {}; }
      o = o || {};
      name = String(name);
      function register(replace) {
        var argv = ['inbox', 'register', name, '--about=' + (o.about || '')];
        if (replace) argv.push('--replace');
        return raw(argv).then(json).then(function (r) { return r.token; });
      }
      var token = await register(!!o.replace);
      var closed = false;
      var active = {};
      async function answer(req) {
        var ctx = {
          cancelled: false,
          oncancel: null,
          progress: function (t) {
            return raw(['inbox', 'progress', token, req.id, '--text=' + String(t)]).catch(function () {});
          }
        };
        active[req.id] = ctx;
        var argv;
        try {
          var out = await handler(req, ctx);
          if (out === undefined || out === null) out = '';
          if (typeof out === 'string') out = { text: out };
          argv = ['inbox', 'reply', token, req.id, '--text=' + String(out.text || '')].concat(filesArg(out.files));
        } catch (e) {
          argv = ['inbox', 'reply', token, req.id, '--text=' + String((e && e.message) || e), '--failed'];
        }
        delete active[req.id];
        if (!ctx.cancelled) await raw(argv).catch(function () {});
      }
      (async function () {
        while (!closed) {
          var r;
          try {
            r = await raw(['inbox', 'next', token, 'files.bin', '--timeout=20000'], 3);
          } catch (e) {
            if (closed) return;
            try { token = await register(false); } catch (e2) { await pause(2000); }
            continue;
          }
          var ev = JSON.parse(r.payload);
          if (ev.type === 'request') {
            ev.files = r.files;
            answer(ev);
          } else if (ev.type === 'cancel' && active[ev.id]) {
            active[ev.id].cancelled = true;
            if (typeof active[ev.id].oncancel === 'function') active[ev.id].oncancel();
          } else if (ev.type === 'gone') {
            return;
          }
        }
      })();
      return {
        name: name,
        close: function () {
          closed = true;
          return raw(['inbox', 'unregister', token]).catch(function () {});
        }
      };
    }
  };
  return ns;
}))JS";

void installInboxNamespace(engine::Engine& engine) {
    // A reload's new realm starts over: the old one's callbacks are dropped
    // (their Persistents leaked rather than freed against a gone realm).
    g_clientCb.clear();
    g_inboxNs = nullptr;
    static bool pumping = false;
    if (!pumping) {
        pumping = true;
        engine.addFramePump(pumpControlClient);
    }
    ev::GlobalValue broG = ev::globalValue("bro");
    if (!broG.found || !ev::isObject(broG.value)) return;
    ev::Persistent bro(broG.value);
    ev::Persistent getter(ev::makeFunction(
        [&engine](Value, std::span<const Value>) -> Value {
            if (g_inboxNs) return g_inboxNs->get();
            auto res = evalScriptJitResult(engine, kInboxSource, "<bro.inbox>");
            if (res.thrown || !ev::isFunction(res.value)) {
                LOG_ERROR("bro.inbox: could not compile the namespace");
                return ev::undefined();
            }
            ev::Persistent factory(res.value);
            ev::Persistent native(ev::createObject());
            ev::Persistent call(ev::makeFunction(
                [&engine](Value, std::span<const Value> a) -> Value { return nativeControlCall(engine, a); }, 4,
                "call"));
            ev::setProperty(native.get(), "call", call.get());
            Value args[1] = {native.get()};
            auto ns = ev::call(factory.get(), ev::undefined(), std::span<const Value>(args, 1));
            if (ns.thrown) {
                LOG_ERROR("bro.inbox: %s", ev::toUtf8(ns.value).c_str());
                return ev::undefined();
            }
            g_inboxNs = new ev::Persistent(ns.value);
            return g_inboxNs->get();
        },
        0, "get inbox"));
    ev::defineAccessor(bro.get(), "inbox", getter.get(), ev::undefined());
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

    installInboxNamespace(engine);

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

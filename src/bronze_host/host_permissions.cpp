// The app's capabilities (engine/app_permissions.h) where the bronze host
// meets them:
//
//   - bronze's access gate (embed/embed_access.h): every native that reads or
//     writes a path, starts a process, connects or listens, captures sound,
//     reads the environment or the user's desktop configuration asks it, in
//     brokit and in every sibling alike (installAccessPolicy);
//   - the terminal's spawn gate and broaudio's engine-level capture gate,
//     which sit below the embedding API;
//   - the privileged bro.<namespace> objects and bro.sys's members: each a
//     proxy that answers from the capability's state at every touch, so a
//     grant given or taken back while the app runs takes effect at once
//     (installGatedNamespaces);
//   - the grant store's watch: a change made by another process (`bro
//     --revoke-app`, the shell's bro.permissions.set) is read within a
//     quarter second, what it took back is revoked, and what is held open
//     under it is closed (installPermissionsPump);
//   - bro.permissions, the shell's view of every app's grants.
//
// The privileged namespaces' sibling natives are reached only through the
// namespace object: none registers a __bro_native entry or a global of its
// own. So the proxy put in a namespace's place is the gate there. An app that
// was never handed the live object has no reference to it to keep.

#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_permissions.h"
#include "bronze_host/host_proxy.h"
#include "engine/app_permissions.h"
#include "engine/capabilities.h"
#include "engine/engine.h"
#include "engine/grant_store.h"
#include "engine/install_record.h"
#if BRO_WITH_TERMINAL
#include "terminal/term_session.h"
#endif
#include "util/log.h"
#include "util/time.h"
#if BRO_WITH_AUDIO
#include <broaudio/capture_gate.h>
#endif

#include "embed/embed_access.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bro::bronze_host {

namespace {

using engine::appPermissions;
using engine::GrantState;

// A function that throws `message` whatever it is called with.
Value throwingFunction(const std::string& message) {
    return ev::makeFunction([message](Value, std::span<const Value>) -> Value {
        return ev::throwAccessError(message);
    }, 0);
}

// ---- the gated namespaces ------------------------------------------------------

// One gated member: `holder[key]` is where it lives (bro.wl is bro["wl"],
// bro.sys.processes is the sys object's "processes").
struct Gate {
    std::string path;   // "wl", "sys.processes": also its capability's name
    std::shared_ptr<ev::Persistent> live;
    // Functions read off the live namespace, wrapped once each: a wrapper
    // asks again at every call, so one kept from before a revocation is
    // refused after it.
    std::map<std::string, std::shared_ptr<ev::Persistent>> wrapped;
};

// `fn` (read off the live namespace as `key`) called with the live namespace
// as its receiver, while the capability is granted.
Value gatedFunction(const std::shared_ptr<Gate>& g, const std::string& key, Value fnIn) {
    auto fn = std::make_shared<ev::Persistent>(fnIn);
    const std::string what = "bro." + g->path + "." + key;
    return ev::makeFunction([g, fn, what](Value, std::span<const Value> args) -> Value {
        const std::string refused = appPermissions().use(g->path, what);
        if (!refused.empty()) return ev::throwAccessError(refused);
        ev::CallResult r = ev::call(fn->get(), g->live->get(), args);
        if (r.thrown) return ev::throwValue(r.value);
        return r.value;
    }, 0);
}

Value liveKeys(Value obj) {
    ev::Persistent target(obj);
    ev::GlobalValue objectCtor = ev::globalValue("Object");
    if (!objectCtor.found) return ev::makeArray(0);
    ev::Persistent ctor(objectCtor.value);
    ev::Persistent keysFn(ev::getProperty(ctor.get(), "keys"));
    const Value args[1] = {target.get()};
    ev::CallResult r = ev::call(keysFn.get(), ctor.get(), std::span<const Value>(args, 1));
    return r.thrown ? ev::makeArray(0) : r.value;
}

// The namespace's stand-in. At every touch it asks the capability's state:
// granted, the touch reaches the live namespace (each function wrapped to
// ask again when called); undecided, the touch asks the user first; denied
// or revoked, it is the unavailable stub (available false, reason why, every
// function throwing it).
Value makeGatedNamespace(const std::string& cap, Value live) {
    auto g = std::make_shared<Gate>();
    g->path = cap;
    g->live = std::make_shared<ev::Persistent>(live);
    HostProxyTraps traps;
    traps.get = [g](const std::string& key, Value& out) -> bool {
        const std::string refused = appPermissions().use(g->path, "bro." + g->path + "." + key);
        if (refused.empty()) {
            Value v = ev::getProperty(g->live->get(), key);
            if (key == "available" && ev::isUndefined(v)) {
                out = ev::fromBool(true);
            } else if (ev::isFunction(v)) {
                auto& slot = g->wrapped[key];
                if (!slot) {
                    ev::Persistent keep(v);
                    slot = std::make_shared<ev::Persistent>(gatedFunction(g, key, keep.get()));
                }
                out = slot->get();
            } else {
                out = v;
            }
            return true;
        }
        if (key == "available") out = ev::fromBool(false);
        else if (key == "reason") out = ev::fromUtf8(ev::refusalMessage(refused));
        else out = throwingFunction(refused);
        return true;
    };
    traps.set = [g](const std::string& key, Value v) {
        // A handler assigned (bro.clip.onchange = f) lands on the live
        // namespace only while granted; refused, it is dropped.
        if (!appPermissions().use(g->path, "bro." + g->path + "." + key, /*mayAsk=*/false).empty()) return;
        ev::Persistent value(v);
        ev::setProperty(g->live->get(), key, value.get());
    };
    traps.has = [g](const std::string& key) -> bool {
        if (key == "available" || key == "reason") return true;
        if (!appPermissions().granted(g->path)) return false;
        return !ev::isUndefined(ev::getProperty(g->live->get(), key));
    };
    traps.ownKeys = [g]() -> std::vector<std::string> {
        std::vector<std::string> keys{"available"};
        if (!appPermissions().granted(g->path)) {
            keys.push_back("reason");
            return keys;
        }
        ev::Persistent arr(liveKeys(g->live->get()));
        const Value lenV = ev::getProperty(arr.get(), "length");
        const uint32_t n = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            Value k = ev::getElement(arr.get(), i);
            if (ev::isString(k) && ev::toUtf8(k) != "available") keys.push_back(ev::toUtf8(k));
        }
        return keys;
    };
    return makeHostProxy(std::move(traps));
}

// holder[key] for capability `cap`: left live when the host grants it (no
// store, prompt or dev mode can take that back), the gated proxy otherwise.
void gateMember(const std::shared_ptr<ev::Persistent>& holder, const std::string& key, const std::string& cap) {
    ev::Persistent live(ev::getProperty(holder->get(), key));
    if (!ev::isObject(live.get())) return;
    if (appPermissions().hostGranted(cap)) return;
    ev::Persistent gated(makeGatedNamespace(cap, live.get()));
    holder->set(ev::setProperty(holder->get(), key, gated.get()));
}

// A namespace this build compiled out is already the unavailable stub; it
// stays that (its reason is the build's, not the app's).
bool compiledOut(Value ns) {
    if (!ev::isObject(ns)) return true;
    ev::Persistent root(ns);
    Value avail = ev::getProperty(root.get(), "available");
    return ev::isBool(avail) && !ev::toBool(avail);
}

// bro.sys, split by member: each of power, audio, network, ... is the
// capability sys.<member>, so the process monitor can list processes without
// being able to power the machine off. The event API on bro.sys itself only
// registers listeners; a service no granted member started raises nothing.
void gateSys(const std::shared_ptr<ev::Persistent>& bro) {
    Value liveIn = ev::getProperty(bro->get(), "sys");
    if (compiledOut(liveIn)) return;
    auto live = std::make_shared<ev::Persistent>(liveIn);
    auto sys = std::make_shared<ev::Persistent>(ev::createObject());
    auto copy = [&](const char* key) {
        ev::Persistent v(ev::getProperty(live->get(), key));
        sys->set(ev::setProperty(sys->get(), key, v.get()));
    };
    for (const char* key : {"on", "off", "addEventListener", "removeEventListener"}) copy(key);
    for (const char* member : engine::sysCapabilityMembers()) {
        copy(member);
        gateMember(sys, member, std::string("sys.") + member);
    }
    // The process table's top-level aliases go with sys.processes, asking
    // at each call.
    for (const char* alias : {"listProcesses", "killProcess"}) {
        if (appPermissions().hostGranted("sys.processes")) { copy(alias); continue; }
        auto fn = std::make_shared<ev::Persistent>(ev::getProperty(live->get(), alias));
        const std::string what = std::string("bro.sys.") + alias;
        ev::Persistent wrapper(ev::makeFunction([fn, live, what](Value, std::span<const Value> args) -> Value {
            const std::string refused = appPermissions().use("sys.processes", what);
            if (!refused.empty()) return ev::throwAccessError(refused);
            ev::CallResult r = ev::call(fn->get(), live->get(), args);
            if (r.thrown) return ev::throwValue(r.value);
            return r.value;
        }, 0, alias));
        sys->set(ev::setProperty(sys->get(), alias, wrapper.get()));
    }
    bro->set(ev::setProperty(bro->get(), "sys", sys->get()));
}

// ---- grant changes while the app runs -------------------------------------------

// bro.permissions' 'change' listeners (main thread only).
std::vector<std::shared_ptr<ev::Persistent>>& changeListeners() {
    static auto* v = new std::vector<std::shared_ptr<ev::Persistent>>();
    return *v;
}

// The store changed (read again by the caller's reload): revalidate what this
// thread holds open, have every other thread do the same at its next turn,
// and tell the shell's listeners.
void afterGrantChange(bool appChanged) {
    if (appChanged) {
        bronze::embed::accessChanged();
        bronze::embed::revalidateHeldAccess();
    }
    if (changeListeners().empty()) return;
    const auto listeners = changeListeners();
    for (const auto& fn : listeners) {
        ev::Persistent evt(ev::createObject());
        {
            ev::Persistent type(ev::fromUtf8("change"));
            evt.set(ev::setProperty(evt.get(), "type", type.get()));
        }
        const Value args[1] = {evt.get()};
        ev::CallResult r = ev::call(fn->get(), ev::undefined(), std::span<const Value>(args, 1));
        if (r.thrown) LOG_WARN("bro.permissions: a 'change' listener threw");
    }
}

// A change this process made itself (bro.permissions.set and the rest): applied
// now, not a quarter second later, so a script sees it on its next line.
void applyOwnChange() { afterGrantChange(appPermissions().reload()); }

// The background check of an installed app's content (install_record.h).
struct InstallVerification {
    std::thread worker;
    std::atomic<bool> done{false};
    std::string actual;   // written by the worker before `done`
};

// ---- bro.permissions -----------------------------------------------------------

Value stringsValue(const std::vector<std::string>& items) {
    return hostArrayOf(items.size(), [&items](size_t i) -> Value { return ev::fromUtf8(items[i]); });
}

Value entryValue(const engine::AppPermissionsEntry& e) {
    ObjectBuilder o;
    o.set("identity", ev::fromUtf8(e.identity));
    o.set("id", ev::fromUtf8(e.id));
    o.set("name", ev::fromUtf8(e.name));
    o.set("dir", ev::fromUtf8(e.dir));
    o.set("installed", ev::fromBool(e.installed));
    o.set("install", ev::fromUtf8(e.install));
    { ev::Persistent v(stringsValue(e.declared)); o.set("declared", v.get()); }
    {
        // Every declared capability, decided or not, and any decision the
        // manifest no longer declares.
        ObjectBuilder d;
        for (const std::string& cap : e.declared) {
            auto it = e.decisions.find(cap);
            const engine::Capability* c = engine::findCapability(cap);
            const char* s = c && c->kind == engine::CapabilityKind::Shell ? "host"
                            : it == e.decisions.end()                    ? "undecided"
                                                                          : engine::decisionName(it->second);
            d.set(cap.c_str(), ev::fromUtf8(s));
        }
        for (const auto& [cap, dec] : e.decisions)
            if (std::find(e.declared.begin(), e.declared.end(), cap) == e.declared.end())
                d.set(cap.c_str(), ev::fromUtf8(engine::decisionName(dec)));
        o.set("decisions", d.get());
    }
    {
        ObjectBuilder u;
        for (const auto& [cap, t] : e.lastUsed) u.set(cap.c_str(), ev::fromDouble(t));
        o.set("lastUsed", u.get());
    }
    { ev::Persistent v(stringsValue(e.picked)); o.set("picked", v.get()); }
    return o.get();
}

// The identity `app` names (an identity, an installed app's id, an app
// folder), or throws.
bool resolveApp(std::span<const Value> a, const char* fn, std::string& identity, std::string& err) {
    if (a.empty() || !ev::isString(a[0])) {
        err = std::string("bro.permissions.") + fn + ": the first argument is the app (identity, id or folder)";
        return false;
    }
    const std::string app = ev::toUtf8(a[0]);
    identity = engine::resolveIdentityArg(app, engine::loadGrantStore());
    if (identity.empty()) {
        err = std::string("bro.permissions.") + fn + ": no app '" + app + "'";
        return false;
    }
    return true;
}

Value makeBroPermissionsValue() {
    ObjectBuilder o;
    // capabilities() -> [{ name, kind: 'ask' | 'shell', summary }]
    o.def("capabilities", 0, [](Value, std::span<const Value>) -> Value {
        const auto cat = engine::capabilityCatalogue();
        return hostArrayOf(cat.size(), [&cat](size_t i) -> Value {
            ObjectBuilder c;
            c.set("name", ev::fromUtf8(cat[i].name));
            c.set("kind", ev::fromUtf8(cat[i].kind == engine::CapabilityKind::Ask ? "ask" : "shell"));
            c.set("summary", ev::fromUtf8(cat[i].summary));
            return c.get();
        });
    });
    // list() -> every installed app and every app the store knows.
    o.def("list", 0, [](Value, std::span<const Value>) -> Value {
        // This process's own uses so far, so its entry is current too.
        appPermissions().flushUsage(/*force=*/true);
        const std::vector<engine::AppPermissionsEntry> all = engine::listAppPermissions();
        return hostArrayOf(all.size(), [&all](size_t i) -> Value { return entryValue(all[i]); });
    });
    // set(app, capability, 'allow' | 'deny' | 'undecided') -> true. `app` is
    // an identity from list(), an installed app's id, or an app folder.
    o.def("set", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !ev::isString(a[0]) || !ev::isString(a[1]) || !ev::isString(a[2]))
            return ev::throwTypeError("bro.permissions.set(app, capability, 'allow' | 'deny' | 'undecided')");
        const std::string cap = ev::toUtf8(a[1]), how = ev::toUtf8(a[2]);
        engine::Decision d;
        if (how == "allow") d = engine::Decision::Allow;
        else if (how == "deny") d = engine::Decision::Deny;
        else if (how == "undecided") d = engine::Decision::Undecided;
        else return ev::throwTypeError("bro.permissions.set: the decision is 'allow', 'deny' or 'undecided'");
        std::string identity, err;
        if (!resolveApp(a, "set", identity, err)) return ev::throwError(err);
        if (!engine::decideAppCapability(identity, {}, cap, d, &err))
            return ev::throwError("bro.permissions.set: " + err);
        applyOwnChange();
        return ev::fromBool(true);
    });
    // reset(app) -> true: every decision and picked path forgotten; it asks again.
    o.def("reset", 1, [](Value, std::span<const Value> a) -> Value {
        std::string identity, err;
        if (!resolveApp(a, "reset", identity, err)) return ev::throwError(err);
        if (!engine::resetAppPermissions(identity, &err)) return ev::throwError("bro.permissions.reset: " + err);
        applyOwnChange();
        return ev::fromBool(true);
    });
    // removePicked(app, path) -> true: one picked file or folder taken back.
    o.def("removePicked", 2, [](Value, std::span<const Value> a) -> Value {
        std::string identity, err;
        if (!resolveApp(a, "removePicked", identity, err)) return ev::throwError(err);
        if (a.size() < 2 || !ev::isString(a[1]))
            return ev::throwTypeError("bro.permissions.removePicked(app, path): path as list() reports it");
        if (!engine::removeAppPicked(identity, ev::toUtf8(a[1]), &err))
            return ev::throwError("bro.permissions.removePicked: " + err);
        applyOwnChange();
        return ev::fromBool(true);
    });
    // on('change', fn) / off('change', fn): the store changed, from any process.
    o.def("on", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isString(a[0]) || ev::toUtf8(a[0]) != "change" || !ev::isFunction(a[1]))
            return ev::throwTypeError("bro.permissions.on('change', listener)");
        changeListeners().push_back(std::make_shared<ev::Persistent>(a[1]));
        return ev::undefined();
    });
    o.def("off", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isFunction(a[1])) return ev::undefined();
        auto& ls = changeListeners();
        for (auto it = ls.begin(); it != ls.end(); ++it) {
            if ((*it)->get().rawBits() == a[1].rawBits()) {
                ls.erase(it);
                break;
            }
        }
        return ev::undefined();
    });
    // devMode: every app granted every ask capability it declares.
    o.accessor("devMode",
        [](Value, std::span<const Value>) -> Value { return ev::fromBool(engine::loadGrantStore().devMode); },
        [](Value, std::span<const Value> a) -> Value {
            std::string err;
            if (!engine::setGrantStoreDevMode(!a.empty() && ev::toBool(a[0]), &err))
                return ev::throwError("bro.permissions.devMode: " + err);
            applyOwnChange();
            return ev::undefined();
        });
    return o.get();
}

}  // namespace

void installAccessPolicy() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    using bronze::embed::AccessKind;
    // One gate for every library's natives (embed/embed_access.h): brokit's
    // fs, child_process, sockets and process.env, and every sibling's paths,
    // launches, captures and desktop configuration.
    bronze::embed::setAccessGate([](const bronze::embed::AccessRequest& r) -> std::string {
        const std::string subject(r.subject);
        const bool mayAsk = !r.held;
        switch (r.kind) {
            case AccessKind::Read: return appPermissions().checkFs(subject, false, mayAsk);
            case AccessKind::Write: return appPermissions().checkFs(subject, true, mayAsk);
            case AccessKind::Spawn: return appPermissions().use("spawn", "start " + subject, mayAsk);
            case AccessKind::Net: return appPermissions().use("net", "connect to " + subject, mayAsk);
            case AccessKind::Capture: return appPermissions().use("mic", subject, mayAsk);
            case AccessKind::EnvRead: return appPermissions().checkEnvRead(subject, mayAsk);
            case AccessKind::EnvWrite: return appPermissions().checkEnvWrite(subject);
            // The user's default apps and recent files are desktop
            // configuration outside the app's own directories: `files`'.
            case AccessKind::DesktopRead: return appPermissions().use("files", subject, mayAsk);
            case AccessKind::DesktopWrite:
                // Its own bro.conf section (its id) is the app's, like its
                // config folder.
                if (appPermissions().ownsConfChange(subject)) return {};
                return appPermissions().use("files", subject, mayAsk);
        }
        return "unknown access";
    });
#if BRO_WITH_TERMINAL
    terminal::setSpawnGate([](const std::string& what) { return appPermissions().use("spawn", what); });
#endif
#if BRO_WITH_AUDIO
    // broaudio's capture paths below the embedding API (the engine's
    // microphone, its taps, loopback) ask this one gate too, so the
    // microphone is gated in the native rather than in each script-facing
    // name for it.
    broaudio::setCaptureGate([](const std::string& what) { return appPermissions().use("mic", what); });
    // And a capture that started is held in the gate's ledger, so taking
    // `mic` back while it runs stops it (revalidateHeldAccess asks Capture).
    broaudio::setCaptureHolder(
        [](const std::string& what, broaudio::CaptureRevoke revoke) -> uint64_t {
            if (!bronze::embed::accessGateActive()) return 0;
            return bronze::embed::holdAccess(AccessKind::Capture, what, std::move(revoke));
        },
        [](uint64_t token) { bronze::embed::releaseAccess(token); });
#endif
}

void installPermissionsPump(engine::Engine& engine) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    auto verify = std::make_shared<InstallVerification>();
    // An installed app's content, hashed in full off the main thread: what
    // the launch's stat check cannot see (install_record.h).
    const std::string hash = appPermissions().installHash();
    if (!hash.empty() && hash != "unrecorded" && hash.rfind("linked:", 0) != 0) {
        const std::string dir = appPermissions().appDir();
        verify->worker = std::thread([verify, dir] {
            const double t0 = util::currentTimeMs();
            verify->actual = engine::hashInstalledTree(dir);
            LOG_INFO("permissions: install content verified in %.1f ms", util::currentTimeMs() - t0);
            verify->done.store(true, std::memory_order_release);
        });
    } else {
        verify->done.store(true, std::memory_order_release);
    }
    auto lastPollMs = std::make_shared<double>(0.0);
    engine.addFramePump([verify, lastPollMs] {
        if (verify->worker.joinable() && verify->done.load(std::memory_order_acquire)) {
            verify->worker.join();
            if (!verify->actual.empty() && verify->actual != appPermissions().installHash()) {
                appPermissions().installChanged(verify->actual);
                afterGrantChange(true);
            }
        }
        // The other threads' turn to revalidate comes at their own loops;
        // this one's is here (a Worker's grant check is the same gate).
        bronze::embed::revalidateHeldAccess();
        const double now = util::currentTimeMs();
        if (now - *lastPollMs < 250.0) return;
        *lastPollMs = now;
        if (appPermissions().storeChanged()) afterGrantChange(appPermissions().reload());
        appPermissions().flushUsage();
    });
    engine.addShutdownHook([verify] {
        if (verify->worker.joinable()) verify->worker.join();
        appPermissions().flushUsage(/*force=*/true);
    });
}

bool isGatedNamespace(const char* name) {
    const std::string cap = engine::capabilityForNamespace(name);
    return !cap.empty() && !appPermissions().hostGranted(cap);
}

void installGatedNamespaces(Value broIn) {
    auto bro = std::make_shared<ev::Persistent>(broIn);
    for (const engine::Capability& c : engine::capabilityCatalogue()) {
        const std::string ns = engine::capabilityForNamespace(c.name);
        if (ns.empty() || ns == "permissions" || ns.rfind("sys.", 0) == 0) continue;
        if (compiledOut(ev::getProperty(bro->get(), ns))) continue;
        gateMember(bro, ns, ns);
    }
    gateSys(bro);
    // bro.permissions exists only for the shell: no one asks for it.
    if (appPermissions().granted("permissions")) {
        ev::Persistent perms(makeBroPermissionsValue());
        bro->set(ev::setProperty(bro->get(), "permissions", perms.get()));
    } else {
        ev::Persistent stub(makeUnavailableNamespace("permissions", appPermissions().use("permissions", {})));
        bro->set(ev::setProperty(bro->get(), "permissions", stub.get()));
    }
}

}  // namespace bro::bronze_host

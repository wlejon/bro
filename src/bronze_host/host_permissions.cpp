// The app's capabilities (engine/app_permissions.h) where the bronze host
// meets them:
//
//   - brokit's access policy: fs, child_process and the network ask the
//     app's permissions at the native, before anything is opened, started or
//     connected (installAccessPolicy);
//   - the terminal's spawn gate (the PTY and the session attach);
//   - the privileged bro.<namespace> objects and bro.sys's members: live,
//     the unavailable stub, or a stub that asks on first touch
//     (installGatedNamespaces);
//   - bro.permissions, the shell's view of every app's grants.
//
// The privileged namespaces' sibling natives are reached only through the
// namespace object: none registers a __bro_native entry or a global of its
// own. So the object put in a namespace's place is the gate there. An app
// that was never handed the live object has no reference to it to keep.

#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_permissions.h"
#include "bronze_host/host_proxy.h"
#include "engine/app_permissions.h"
#include "engine/capabilities.h"
#include "engine/grant_store.h"
#if BRO_WITH_TERMINAL
#include "terminal/term_session.h"
#endif
#include "util/log.h"
#if BRO_WITH_AUDIO
#include <broaudio/capture_gate.h>
#endif
#if BRO_WITH_SEARCH
#include <brosearch/api.h>
#endif
#if BRO_WITH_APPS
#include <broapps/api.h>
#endif
#if BRO_WITH_VFS
#include <brovfs/api.h>
#endif
#if BRO_WITH_THUMB
#include <brothumb/api.h>
#endif

#include "api/api.h"  // brokit::api::setAccessPolicy

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

using engine::appPermissions;
using engine::GrantState;

// A function that throws `message` whatever it is called with.
Value throwingFunction(const std::string& message) {
    return ev::makeFunction([message](Value, std::span<const Value>) -> Value {
        return ev::throwError(message);
    }, 0);
}

// `fn` called with `self` as its receiver, whatever receiver the caller
// used: what the asking stub hands out, since a member read off the stub is
// then called with the stub as `this`.
Value boundTo(const std::shared_ptr<ev::Persistent>& self, Value fnIn) {
    auto fn = std::make_shared<ev::Persistent>(fnIn);
    return ev::makeFunction([self, fn](Value, std::span<const Value> args) -> Value {
        ev::CallResult r = ev::call(fn->get(), self->get(), args);
        if (r.thrown) return ev::throwValue(r.value);
        return r.value;
    }, 0);
}

// One gated member: `holder[key]` is where it lives (bro.wl is bro["wl"],
// bro.sys.processes is the sys object's "processes").
struct Gate {
    std::string path;   // "wl", "sys.processes": also its capability's name
    std::string key;
    std::shared_ptr<ev::Persistent> holder;
    std::shared_ptr<ev::Persistent> live;
};

void putInHolder(const Gate& g, Value v) {
    ev::Persistent value(v);
    g.holder->set(ev::setProperty(g.holder->get(), g.key, value.get()));
}

// The stand-in for a declared, undecided namespace: its first touch asks.
// Allowed, the live namespace takes its place and the touch is answered from
// it; denied, the unavailable stub does. Unanswered (a headless run that set
// no answer), the touch is refused and the next one asks again.
Value makeAskingNamespace(const Gate& gate) {
    auto g = std::make_shared<Gate>(gate);
    HostProxyTraps traps;
    traps.get = [g](const std::string& key, Value& out) -> bool {
        const std::string refused = appPermissions().use(g->path, "bro." + g->path + "." + key);
        if (refused.empty()) {
            putInHolder(*g, g->live->get());
            Value v = ev::getProperty(g->live->get(), key);
            out = ev::isFunction(v) ? boundTo(g->live, v) : v;
            return true;
        }
        if (appPermissions().state(g->path) == GrantState::Denied) {
            ev::Persistent stub(makeUnavailableNamespace(g->path, refused));
            putInHolder(*g, stub.get());
        }
        if (key == "available") out = ev::fromBool(false);
        else if (key == "reason") out = ev::fromUtf8(refused);
        else out = throwingFunction("bro." + g->path + "." + key + ": " + refused);
        return true;
    };
    traps.has = [](const std::string& key) -> bool { return key == "available" || key == "reason"; };
    traps.ownKeys = []() -> std::vector<std::string> { return {"available", "reason"}; };
    return makeHostProxy(std::move(traps));
}

// holder[key] for capability `cap`: left live when granted, the asking stub
// when undecided, the unavailable stub (saying why) when denied.
void gateMember(const std::shared_ptr<ev::Persistent>& holder, const std::string& key, const std::string& cap) {
    Value live = ev::getProperty(holder->get(), key);
    if (!ev::isObject(live)) return;
    const GrantState st = appPermissions().state(cap);
    if (st == GrantState::Granted) return;
    Gate g{cap, key, holder, std::make_shared<ev::Persistent>(live)};
    if (st == GrantState::Undecided) {
        ev::Persistent asking(makeAskingNamespace(g));
        putInHolder(g, asking.get());
        return;
    }
    // Denied: use() asks nothing for a denied capability; it only words why.
    ev::Persistent stub(makeUnavailableNamespace(cap, appPermissions().use(cap, {})));
    putInHolder(g, stub.get());
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
    // The process table's top-level aliases go with sys.processes.
    for (const char* alias : {"listProcesses", "killProcess"}) {
        if (appPermissions().granted("sys.processes")) { copy(alias); continue; }
        auto fn = std::make_shared<ev::Persistent>(ev::getProperty(live->get(), alias));
        const std::string what = std::string("bro.sys.") + alias;
        ev::Persistent wrapper(ev::makeFunction([fn, live, what](Value, std::span<const Value> args) -> Value {
            const std::string refused = appPermissions().use("sys.processes", what);
            if (!refused.empty()) return ev::throwError(what + ": " + refused);
            ev::CallResult r = ev::call(fn->get(), live->get(), args);
            if (r.thrown) return ev::throwValue(r.value);
            return r.value;
        }, 0, alias));
        sys->set(ev::setProperty(sys->get(), alias, wrapper.get()));
    }
    bro->set(ev::setProperty(bro->get(), "sys", sys->get()));
}

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
    { ev::Persistent v(stringsValue(e.picked)); o.set("picked", v.get()); }
    return o.get();
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
        const std::vector<engine::AppPermissionsEntry> all = engine::listAppPermissions();
        return hostArrayOf(all.size(), [&all](size_t i) -> Value { return entryValue(all[i]); });
    });
    // set(app, capability, 'allow' | 'deny' | 'undecided') -> true. `app` is
    // an identity from list(), an installed app's id, or an app folder.
    o.def("set", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !ev::isString(a[0]) || !ev::isString(a[1]) || !ev::isString(a[2]))
            return ev::throwTypeError("bro.permissions.set(app, capability, 'allow' | 'deny' | 'undecided')");
        const std::string app = ev::toUtf8(a[0]), cap = ev::toUtf8(a[1]), how = ev::toUtf8(a[2]);
        engine::Decision d;
        if (how == "allow") d = engine::Decision::Allow;
        else if (how == "deny") d = engine::Decision::Deny;
        else if (how == "undecided") d = engine::Decision::Undecided;
        else return ev::throwTypeError("bro.permissions.set: the decision is 'allow', 'deny' or 'undecided'");
        const engine::GrantStoreData store = engine::loadGrantStore();
        const std::string identity = engine::resolveIdentityArg(app, store);
        if (identity.empty()) return ev::throwError("bro.permissions.set: no app '" + app + "'");
        std::string err;
        if (!engine::decideAppCapability(identity, {}, cap, d, &err))
            return ev::throwError("bro.permissions.set: " + err);
        return ev::fromBool(true);
    });
    // devMode: every app granted every ask capability it declares.
    o.accessor("devMode",
        [](Value, std::span<const Value>) -> Value { return ev::fromBool(engine::loadGrantStore().devMode); },
        [](Value, std::span<const Value> a) -> Value {
            std::string err;
            if (!engine::setGrantStoreDevMode(!a.empty() && ev::toBool(a[0]), &err))
                return ev::throwError("bro.permissions.devMode: " + err);
            return ev::undefined();
        });
    return o.get();
}

}  // namespace

void installAccessPolicy() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    brokit::api::AccessPolicy policy;
    policy.fs = [](const std::string& path, brokit::api::FsAccess access) {
        return appPermissions().checkFs(path, access == brokit::api::FsAccess::Write);
    };
    policy.spawn = [](const std::string& command) { return appPermissions().use("spawn", "start " + command); };
    policy.net = [](const std::string& url) { return appPermissions().use("net", "connect to " + url); };
    brokit::api::setAccessPolicy(std::move(policy));
#if BRO_WITH_TERMINAL
    terminal::setSpawnGate([](const std::string& what) { return appPermissions().use("spawn", what); });
#endif
#if BRO_WITH_AUDIO
    // broaudio's capture paths (the engine's microphone, its taps, loopback)
    // and the surfaces over them (getUserMedia, bro.mic, bro.listen) all ask
    // this one gate, so the microphone is gated in the native rather than in
    // each script-facing name for it.
    broaudio::setCaptureGate([](const std::string& what) { return appPermissions().use("mic", what); });
#endif
    // The desktop siblings that take paths or start programs natively, outside
    // brokit: the same rules as `fs` and child_process, so bro.vfs cannot
    // delete, bro.search read, or bro.apps launch what require('fs') or
    // child_process would be refused.
#if BRO_WITH_VFS
    brovfs::api::setPathGate([](const std::string& path, brovfs::api::PathAccess a) {
        return appPermissions().checkFs(path, a == brovfs::api::PathAccess::Write);
    });
#endif
#if BRO_WITH_SEARCH
    brosearch::api::setPathGate([](const std::string& path, brosearch::api::PathAccess a) {
        return appPermissions().checkFs(path, a == brosearch::api::PathAccess::Write);
    });
#endif
#if BRO_WITH_THUMB
    brothumb::api::setPathGate([](const std::string& path, brothumb::api::PathAccess a) {
        return appPermissions().checkFs(path, a == brothumb::api::PathAccess::Write);
    });
#endif
#if BRO_WITH_APPS
    broapps::api::setSpawnGate([](const std::string& what) { return appPermissions().use("spawn", what); });
    // The user's default apps and recent files are desktop configuration
    // outside the app's own directories: the `files` capability's.
    broapps::api::setConfigWriteGate([](const std::string& what) { return appPermissions().use("files", what); });
#endif
}

bool isAskingNamespace(const char* name) {
    const std::string cap = engine::capabilityForNamespace(name);
    return !cap.empty() && appPermissions().state(cap) == GrantState::Undecided;
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

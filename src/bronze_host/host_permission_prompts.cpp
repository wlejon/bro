// bro.permissions' prompt requests: the shell's end of the session's
// permission prompts (platform/permission_prompts.h, docs/apps.md "Asking").
//
// The shell answers allow or deny and nothing else: the app that asked keeps
// the decision for its own identity, so this side never writes a grant on a
// requester's say-so. A request names a capability by name; the summary shown
// is this process's own catalogue's, never the requester's text, and a name
// that is not an ask capability is dropped unseen.

#include "bronze_host/host_permission_prompts.h"

#include "bronze_host/host_builder.h"
#include "engine/app_manifest.h"
#include "engine/capabilities.h"
#include "platform/permission_prompts.h"
#include "util/log.h"

#include <map>
#include <memory>
#include <vector>

namespace bro::bronze_host {

namespace {

using Listeners = std::vector<std::shared_ptr<ev::Persistent>>;

struct PromptState {
    platform::prompts::Server server;
    Listeners onRequest, onCancel;
    // Delivered and not yet answered, dropped or cancelled: the request objects.
    std::map<uint64_t, std::shared_ptr<ev::Persistent>> outstanding;
    ev::Realm* realm = nullptr;   // the realm the listeners belong to
};

PromptState& state() {
    static auto* s = new PromptState();
    return *s;
}

// The listeners' realm is going (a reload): stop serving, so the apps asking
// get no answer, and let go of its values without freeing them against it.
void forgetRealm(ev::Realm* r) {
    PromptState& s = state();
    if (!r || s.realm != r) return;
    s.server.stop();
    static auto* dropped = new std::vector<std::shared_ptr<ev::Persistent>>();
    for (auto* ls : {&s.onRequest, &s.onCancel})
        for (auto& fn : *ls) dropped->push_back(std::move(fn));
    for (auto& [id, req] : s.outstanding) dropped->push_back(std::move(req));
    s.onRequest.clear();
    s.onCancel.clear();
    s.outstanding.clear();
    s.realm = nullptr;
}

void noteRealm() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        ev::addRealmDestroyHook(&forgetRealm);
    }
    state().realm = ev::currentRealm();
}

void callAll(const Listeners& listeners, Value arg, const char* type) {
    ev::Persistent keep(arg);
    const Listeners copy = listeners;   // a listener may subscribe or unsubscribe
    for (const auto& fn : copy) {
        const Value args[1] = {keep.get()};
        ev::CallResult r = ev::call(fn->get(), ev::undefined(), std::span<const Value>(args, 1));
        if (r.thrown) LOG_WARN("bro.permissions: a '%s' listener threw", type);
    }
}

// The request `id` is settled: no longer outstanding, marked on its object.
void settle(uint64_t id, const char* how) {
    auto& out = state().outstanding;
    auto it = out.find(id);
    if (it == out.end()) return;
    std::shared_ptr<ev::Persistent> req = it->second;
    out.erase(it);
    ev::Persistent v(ev::fromUtf8(how));
    req->set(ev::setProperty(req->get(), "settled", v.get()));
}

void fireCancel(uint64_t id) {
    auto& out = state().outstanding;
    auto it = out.find(id);
    if (it == out.end()) return;
    std::shared_ptr<ev::Persistent> req = it->second;
    settle(id, "cancelled");
    ObjectBuilder e;
    e.set("type", ev::fromUtf8("cancel"));
    e.set("id", ev::fromDouble(static_cast<double>(id)));
    e.set("request", req->get());
    callAll(state().onCancel, e.get(), "cancel");
}

Value requestValue(const platform::prompts::Server::Event& ev_, const engine::Capability& cap) {
    const uint64_t id = ev_.id;
    const platform::prompts::Question& q = ev_.question;
    ObjectBuilder o;
    o.set("type", ev::fromUtf8("request"));
    o.set("id", ev::fromDouble(static_cast<double>(id)));
    {
        ObjectBuilder app;
        app.set("name", ev::fromUtf8(q.appName.empty() ? q.appId : q.appName));
        app.set("id", ev::fromUtf8(q.appId));
        app.set("identity", ev::fromUtf8(q.identity));
        app.set("dir", ev::fromUtf8(q.dir));
        app.set("pid", ev::fromDouble(static_cast<double>(ev_.peerPid)));
        o.set("app", app.get());
    }
    o.set("capability", ev::fromUtf8(cap.name));
    o.set("summary", ev::fromUtf8(cap.summary));
    o.set("what", ev::fromUtf8(q.what));
    o.set("settled", ev::fromUtf8(""));
    // respond(true | false): the user's answer; null drops it with none.
    o.def("respond", 1, [id](Value, std::span<const Value> a) -> Value {
        if (!state().outstanding.count(id)) return ev::fromBool(false);
        const bool none = a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0]);
        const bool allow = !none && ev::toBool(a[0]);
        const bool ok = none ? state().server.drop(id) : state().server.respond(id, allow);
        settle(id, none ? "dropped" : allow ? "allowed" : "denied");
        return ev::fromBool(ok);
    });
    return o.get();
}

void startServing() {
    PromptState& s = state();
    if (s.server.running()) return;
    std::string why;
    if (!s.server.start(platform::prompts::endpointName(engine::scratchProfileHome()), &why))
        LOG_WARN("bro.permissions: not serving app permission prompts: %s", why.c_str());
}

void stopServing() {
    PromptState& s = state();
    if (!s.server.running()) return;
    s.server.stop();
    // Each asker's connection closed with no answer: to the shell, every
    // outstanding request is cancelled.
    std::vector<uint64_t> ids;
    for (const auto& [id, req] : s.outstanding) ids.push_back(id);
    for (uint64_t id : ids) fireCancel(id);
}

bool removeFrom(Listeners& ls, Value fn) {
    for (auto it = ls.begin(); it != ls.end(); ++it) {
        if ((*it)->get().rawBits() == fn.rawBits()) {
            ls.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace

bool addPromptListener(const std::string& type, Value fn) {
    PromptState& s = state();
    if (type != "request" && type != "cancel") return false;
    noteRealm();
    (type == "request" ? s.onRequest : s.onCancel).push_back(std::make_shared<ev::Persistent>(fn));
    if (type == "request") startServing();
    return true;
}

bool removePromptListener(const std::string& type, Value fn) {
    PromptState& s = state();
    if (type == "request") {
        removeFrom(s.onRequest, fn);
        if (s.onRequest.empty()) stopServing();
        return true;
    }
    if (type == "cancel") {
        removeFrom(s.onCancel, fn);
        return true;
    }
    return false;
}

bool servingPermissionPrompts() { return state().server.running(); }

void pumpPermissionPrompts() {
    PromptState& s = state();
    if (!s.server.running()) return;
    for (platform::prompts::Server::Event& e : s.server.take()) {
        if (e.kind == platform::prompts::Server::Event::Kind::Cancel) {
            fireCancel(e.id);
            continue;
        }
        const engine::Capability* cap = engine::findCapability(e.question.capability);
        if (!cap || cap->kind != engine::CapabilityKind::Ask) {
            LOG_WARN("bro.permissions: dropped a prompt for \"%s\", which is not a capability an app asks for",
                     e.question.capability.c_str());
            s.server.drop(e.id);
            continue;
        }
        auto req = std::make_shared<ev::Persistent>(requestValue(e, *cap));
        s.outstanding[e.id] = req;
        callAll(s.onRequest, req->get(), "request");
    }
}

void stopPermissionPrompts() {
    PromptState& s = state();
    s.server.stop();
    s.outstanding.clear();
    s.onRequest.clear();
    s.onCancel.clear();
}

}  // namespace bro::bronze_host

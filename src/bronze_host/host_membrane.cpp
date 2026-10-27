// The membrane between compartments (host_membrane.h): window proxies that
// carry only postMessage, and capabilities whose calls clone their arguments
// and results across.

#include "bronze_host/host_membrane.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_window_message.h"
#include "bronze_host/host_worker_msg.h"
#include "util/log.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---- window proxies -----------------------------------------------------------

struct ProxyEntry {
    uint64_t viewer = 0, target = 0;
    ev::Persistent proxy;
};
std::vector<ProxyEntry> s_proxies;

// The document a post to `c` is delivered at: the app's is null (the engine's).
dom::Document* deliveryDocument(const Compartment* c) {
    return c->kind == CompartmentKind::App ? nullptr : c->document;
}

Value makeWindowProxy(uint64_t viewerId, uint64_t targetId) {
    ObjectBuilder b;
    b.def("postMessage", 2, [viewerId, targetId](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("Window.postMessage: 1 argument required, but only 0 present");
        ev::Persistent message(a[0]);
        PostMessageTarget target;
        parsePostMessageArgs(a, "Window.postMessage", false, target);
        auto msg = std::make_shared<Message>();
        serializeForPost(message, target.transfer, *msg);
        if (!target.deliver) return ev::undefined();
        // The sender, as the receiver will see it: the compartment that holds
        // this proxy (its viewer), whose window the event's `source` stands
        // for.
        postHostTask([viewerId, targetId, msg]() {
            Compartment* to = compartmentById(targetId);
            if (!to || to->dying) return;
            deliverWindowMessage(deliveryDocument(to), msg,
                                 [viewerId]() -> Value {
                                     Compartment* from = compartmentById(viewerId);
                                     return from && !from->dying ? windowProxyFor(from) : ev::null();
                                 },
                                 kHostPageOrigin);
        });
        return ev::undefined();
    });
    b.accessor("closed",
               [targetId](Value, std::span<const Value>) -> Value {
                   Compartment* c = compartmentById(targetId);
                   return ev::fromBool(!c || c->dying);
               },
               nullptr);
    b.def("focus", 0, [](Value, std::span<const Value>) -> Value { return ev::undefined(); });
    b.def("blur", 0, [](Value, std::span<const Value>) -> Value { return ev::undefined(); });
    return b.get();
}

// ---- capabilities -------------------------------------------------------------

struct CapEntry {
    uint64_t owner = 0, to = 0;
    ev::Persistent fn;   // owner's function
    ev::Persistent cap;  // the capability, of `to`'s realm
};
std::vector<std::shared_ptr<CapEntry>> s_caps;

// The `bro.app.host` object of each appview compartment.
std::unordered_map<uint64_t, std::unique_ptr<ev::Persistent>> s_hostObjects;

// A value on its way out of one realm: its clone as bytes, or the function
// that becomes a capability on the other side, or what refused the clone.
struct Crossing {
    bool isFunction = false;
    ev::Persistent fn;
    Message msg;
    bool failed = false;
    std::string failure;
};

std::string describeThrown(Value v) {
    ev::Persistent held(v);
    if (ev::isObject(held.get())) {
        Value m = ev::getProperty(held.get(), "message");
        if (ev::isString(m)) return ev::toUtf8(m);
    }
    return ev::isString(held.get()) ? ev::toUtf8(held.get()) : std::string("a value that could not be cloned");
}

// In the realm the value belongs to.
void crossOut(Value v, Crossing& out) {
    ev::Persistent held(v);
    if (ev::isFunction(held.get())) {
        out.isFunction = true;
        out.fn.set(held.get());
        return;
    }
    ev::CallResult r = ev::catchThrow([&]() -> Value {
        if (!serializeMessage(held.get(), {}, out.msg)) {
            out.failed = true;
            if (out.failure.empty()) out.failure = "the value could not be cloned";
        }
        return ev::undefined();
    });
    if (r.thrown) {
        out.failed = true;
        out.failure = describeThrown(r.value);
    }
}

// In the receiving realm (`to`, entered): the clone read back, or the
// capability for a function of `from`'s.
Value crossIn(Crossing& c, Compartment* from, Compartment* to) {
    if (c.isFunction) return makeCapability(from, c.fn.get(), to);
    if (c.failed) return ev::throwValue(hostMakeDomError("DataCloneError", c.failure));
    return deserializeMessage(c.msg);
}

std::string stoppedText(const Compartment* c) {
    return (c && !c->name.empty() ? c->name : std::string("the app")) + " has stopped";
}

// One call through a capability: in the caller's realm (`to`), into the
// owner's, and back.
Value callCapability(const std::shared_ptr<CapEntry>& e, std::span<const Value> argsIn) {
    Compartment* owner = compartmentById(e->owner);
    Compartment* caller = compartmentById(e->to);
    if (!owner || owner->dying || ev::isUndefined(e->fn.get())) return ev::throwTypeError(stoppedText(owner));
    if (!caller) caller = currentCompartment();

    std::vector<Crossing> args(argsIn.size());
    {
        std::vector<ev::Persistent> held;
        held.reserve(argsIn.size());
        for (Value v : argsIn) held.emplace_back(v);
        for (size_t i = 0; i < held.size(); ++i) {
            crossOut(held[i].get(), args[i]);
            if (args[i].failed)
                return ev::throwValue(hostMakeDomError("DataCloneError", "argument " + std::to_string(i + 1) +
                                                                             ": " + args[i].failure));
        }
    }
    // The caller's promise, made here, for an owner's function that answers
    // with one.
    ev::Persistent pending(ev::undefined());
    Crossing result;
    bool thrown = false;
    bool async = false;
    bool done = false;
    // Said before the call: a stopped owner may be gone after it.
    const std::string stopped = stoppedText(owner);
    // A sync entry of the owner's: on its fiber when it is an app with one,
    // which must answer within the budget or is stopped.
    const bool entered = runEntry(owner, EntryKind::Sync, [&] {
        std::vector<ev::Persistent> argv;
        argv.reserve(args.size());
        for (auto& a : args) {
            ev::CallResult in = ev::catchThrow([&]() -> Value { return crossIn(a, caller, owner); });
            argv.emplace_back(in.thrown ? ev::undefined() : in.value);
        }
        std::vector<Value> raw;
        raw.reserve(argv.size());
        for (auto& a : argv) raw.push_back(a.get());
        ev::CallResult r = ev::call(e->fn.get(), ev::undefined(), std::span<const Value>(raw.data(), raw.size()));
        ev::Persistent value(r.value);
        if (r.thrown) {
            thrown = true;
            crossOut(value.get(), result);
            if (result.failed) {
                result.failed = false;
                result.isFunction = false;
                Message m;
                ev::Persistent text(ev::fromUtf8(describeThrown(value.get())));
                serializeMessage(text.get(), {}, m);
                result.msg = std::move(m);
            }
        } else if (ev::isPromise(value.get())) {
            async = true;
            // Settle the caller's promise with the clone of what the owner's
            // settles with. The reactions run as the owner's jobs (they were
            // subscribed here), then enter the caller to settle.
            auto settle = std::make_shared<ev::Persistent>();
            {
                CompartmentScope back(caller);
                settle->set(ev::createPromise());
                pending.set(settle->get());
            }
            const uint64_t callerId = caller->id, ownerId = owner->id;
            auto onSettle = [settle, callerId, ownerId](bool ok) {
                return [settle, callerId, ownerId, ok](Value, std::span<const Value> a) -> Value {
                    Crossing c;
                    crossOut(a.empty() ? ev::undefined() : a[0], c);
                    Compartment* to = compartmentById(callerId);
                    Compartment* from = compartmentById(ownerId);
                    if (!to || !from) return ev::undefined();
                    CompartmentScope s(to);
                    if (!s.entered()) return ev::undefined();
                    ev::CallResult v = ev::catchThrow([&]() -> Value { return crossIn(c, from, to); });
                    if (v.thrown || !ok)
                        ev::rejectPromise(settle->get(), v.value);
                    else
                        ev::resolvePromise(settle->get(), v.value);
                    return ev::undefined();
                };
            };
            ev::Persistent okFn(ev::makeFunction(onSettle(true), 1));
            ev::Persistent errFn(ev::makeFunction(onSettle(false), 1));
            ev::Persistent thenFn(ev::getProperty(value.get(), "then"));
            Value targs[2] = {okFn.get(), errFn.get()};
            ev::call(thenFn.get(), value.get(), std::span<const Value>(targs, 2));
        } else {
            crossOut(value.get(), result);
        }
        done = true;
    });
    // Not entered, or stopped before it answered.
    if (!entered || !done) return ev::throwTypeError(stopped);
    if (async) return pending.get();
    ev::CallResult back = ev::catchThrow([&]() -> Value { return crossIn(result, owner, caller); });
    if (thrown || back.thrown) return ev::throwValue(back.value);
    return back.value;
}

}  // namespace

Value windowProxyFor(Compartment* target) {
    if (!target) return ev::null();
    // The app's own frames reach each other's windows themselves, as the
    // web's same-origin frames do: one app, one set of grants, nothing a
    // membrane would contain.
    if (sameApp(currentCompartment(), target) && target->realm) return ev::getRealmGlobal(target->realm);
    const uint64_t viewer = currentCompartmentId();
    for (auto& p : s_proxies)
        if (p.viewer == viewer && p.target == target->id) return p.proxy.get();
    ev::Persistent proxy(makeWindowProxy(viewer, target->id));
    s_proxies.push_back({viewer, target->id, ev::Persistent(proxy.get())});
    return s_proxies.back().proxy.get();
}

void installParentWindowProxy(Compartment& c) {
    if (!c.parent) return;
    Compartment* top = c.parent;
    while (top->parent) top = top->parent;
    ev::Persistent parent(windowProxyFor(c.parent));
    ev::Persistent topP(windowProxyFor(top));
    // The element it is shown in, to the app's own frames only: another app
    // sees null, as a cross-origin frame does.
    ev::Persistent frameElement(ev::null());
    if (c.hostElement && sameApp(&c, c.parent)) {
        CompartmentScope scope(c.parent);
        if (scope.entered()) frameElement.set(hostElementValue(c.hostElement));
    }
    ev::registerGlobal("parent", parent.get());
    ev::registerGlobal("top", topP.get());
    ev::registerGlobal("frameElement", frameElement.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::Persistent global(gt.value);
        ev::setProperty(global.get(), "parent", parent.get());
        ev::setProperty(global.get(), "top", topP.get());
        ev::setProperty(global.get(), "frameElement", frameElement.get());
    }
}

void clearWindowProxies(uint64_t compartmentId) {
    if (compartmentId == 0) {
        s_proxies.clear();
        for (auto& e : s_caps) e->fn.set(ev::undefined());
        s_caps.clear();
        s_hostObjects.clear();
        return;
    }
    std::erase_if(s_proxies, [compartmentId](const ProxyEntry& p) {
        return p.viewer == compartmentId || p.target == compartmentId;
    });
    // A capability either side of a gone compartment refuses from now on: the
    // function it held is let go.
    std::erase_if(s_caps, [compartmentId](const std::shared_ptr<CapEntry>& e) {
        if (e->owner != compartmentId && e->to != compartmentId) return false;
        e->fn.set(ev::undefined());
        return true;
    });
    s_hostObjects.erase(compartmentId);
}

Value makeCapability(Compartment* owner, Value fnIn, Compartment* to) {
    if (!owner || !to) return ev::undefined();
    ev::Persistent fn(fnIn);
    for (auto& e : s_caps)
        if (e->owner == owner->id && e->to == to->id && ev::toBits(e->fn.get()) == ev::toBits(fn.get()))
            return e->cap.get();
    auto entry = std::make_shared<CapEntry>();
    entry->owner = owner->id;
    entry->to = to->id;
    entry->fn.set(fn.get());
    {
        CompartmentScope scope(to);
        std::weak_ptr<CapEntry> weak = entry;
        entry->cap.set(ev::makeFunction(
            [weak](Value, std::span<const Value> a) -> Value {
                std::shared_ptr<CapEntry> e = weak.lock();
                if (!e) return ev::throwTypeError("this capability was revoked");
                return callCapability(e, a);
            },
            0));
    }
    s_caps.push_back(entry);
    return entry->cap.get();
}

Value cloneAcross(Value v, Compartment* from, Compartment* to) {
    Crossing c;
    {
        CompartmentScope scope(from);
        crossOut(v, c);
    }
    CompartmentScope scope(to);
    return crossIn(c, from, to);
}

void installHostCapabilities(Compartment& c, Value broApp) {
    ev::Persistent app(broApp);
    ev::Persistent host(ev::createObject());
    if (ev::isObject(app.get())) ev::setProperty(app.get(), "host", host.get());
    s_hostObjects[c.id] = std::make_unique<ev::Persistent>(host.get());
}

void exposeCapability(Compartment* owner, Compartment* to, const std::string& name, Value fnIn) {
    if (!owner || !to || to->dying) return;
    ev::Persistent fn(fnIn);
    auto it = s_hostObjects.find(to->id);
    if (it == s_hostObjects.end()) return;
    ev::Persistent cap(makeCapability(owner, fn.get(), to));
    CompartmentScope scope(to);
    if (!scope.entered()) return;
    ev::setProperty(it->second->get(), name, cap.get());
}

}  // namespace bro::bronze_host

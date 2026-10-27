// Compartments: the registry, entry, and teardown (host_compartment.h).
//
// Everything here is the main thread's: compartments are made, entered and
// destroyed on the page thread only. A Worker's thread has realms of its own
// and no compartments; asked there, currentCompartment answers the app's, and
// the Worker's grants are the ones it was started under (host_worker.cpp).

#include "bronze_host/host_compartment.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_appview.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_membrane.h"
#include "bronze_host/host_runtime.h"
#include "engine/app_permissions.h"
#include "engine/app_runtime.h"
#include "engine/engine.h"
#include "util/log.h"

#include <embed/embed_access.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace bro::bronze_host {

namespace {

std::thread::id g_mainThread = std::this_thread::get_id();
bool g_mainThreadSet = false;

Compartment& appSlot() {
    static Compartment* app = [] {
        auto* c = new Compartment();
        c->id = 0;
        c->kind = CompartmentKind::App;
        c->permissions = &engine::processPermissions();
        return c;
    }();
    return *app;
}

// Never freed while live; a destroyed one is moved to s_dead until its realm
// goes, then deleted.
std::vector<std::unique_ptr<Compartment>> s_live;
std::vector<std::unique_ptr<Compartment>> s_dead;
std::unordered_map<const dom::Document*, Compartment*> s_byDocument;
std::unordered_map<const ev::Realm*, Compartment*> s_byRealm;
// Documents whose compartment was torn down while the engine still holds
// them (an app the runtime stopped, until its page's seam stops the view):
// they answer the dead slot, which nothing enters, never the app's.
std::unordered_set<const dom::Document*> s_deadDocuments;

Compartment& deadSlot() {
    static Compartment* dead = [] {
        auto* c = new Compartment();
        c->id = UINT64_MAX;
        c->kind = CompartmentKind::Frame;
        c->name = "(stopped)";
        c->permissions = &engine::processPermissions();
        c->dying = true;
        return c;
    }();
    return *dead;
}
uint64_t s_nextId = 1;

bool onMainThread() { return !g_mainThreadSet || std::this_thread::get_id() == g_mainThread; }

// The realms this thread has entered through a CompartmentScope, innermost
// last: a dying compartment's realm is destroyed only once it is off this
// stack and off the realm stack.
std::vector<ev::Realm*> s_entered;

// firstInRealm's record: per realm, the keys asked so far.
thread_local std::unordered_map<const ev::Realm*, std::unordered_set<const void*>> t_realmKeys;

void forgetRealmKeys(ev::Realm* realm) { t_realmKeys.erase(realm); }

std::vector<PerCompartment*>& perCompartmentSlots();  // below

// On its realm stack, on the host's scope stack, or with an entry running on
// its fiber: its realm must outlive this turn.
bool compartmentInUse(const Compartment* c) {
    if (c->fiberDepth > 0) return true;
    if (ev::currentRealm() == c->realm) return true;
    return std::find(s_entered.begin(), s_entered.end(), c->realm) != s_entered.end();
}

// The permissions resolver (engine/app_permissions.h): the current
// compartment's grants on the page thread, the Worker's own on its thread.
engine::AppPermissions* resolvePermissions() {
    if (!onMainThread()) return nullptr;
    Compartment* c = currentCompartment();
    return c ? c->permissions : nullptr;
}

void finishDestroy(std::unique_ptr<Compartment> owned) {
    Compartment* c = owned.get();
    c->queued.clear();
    if (c->fiber) {
        // A job suspended on it is unwound first, while its realm still
        // stands (terminated: nothing the app can catch runs).
        ev::destroyCompartment(c->fiber);
        c->fiber = nullptr;
    }
    if (c->realm) {
        s_byRealm.erase(c->realm);
        forgetRealmKeys(c->realm);
        ev::destroyRealm(c->realm);
        c->realm = nullptr;
    }
}

}  // namespace

const char* compartmentKindName(CompartmentKind kind) {
    switch (kind) {
        case CompartmentKind::App: return "app";
        case CompartmentKind::Frame: return "iframe";
        case CompartmentKind::Window: return "window";
        case CompartmentKind::Panel: return "panel";
        case CompartmentKind::AppView: return "appview";
    }
    return "?";
}

void initAppCompartment(engine::Engine& engine) {
    g_mainThread = std::this_thread::get_id();
    g_mainThreadSet = true;
    Compartment& app = appSlot();
    app.realm = ev::currentRealm();
    app.document = nullptr;
    app.permissions = &engine::processPermissions();
    app.name = app.permissions->appName();
    app.appDir = engine.appDir();
    app.basePath = engine.appDir();
    s_byRealm[app.realm] = &app;
    engine::setPermissionsResolver(&resolvePermissions);
    ev::addRealmDestroyHook(&forgetRealmKeys);
}

Compartment& appCompartment() { return appSlot(); }

Compartment* currentCompartment() {
    if (!onMainThread()) return &appSlot();
    auto it = s_byRealm.find(ev::currentRealm());
    return it != s_byRealm.end() ? it->second : &appSlot();
}

bool inChildCompartment() { return currentCompartment()->kind != CompartmentKind::App; }

const engine::AppRuntimeInfo& compartmentApp() {
    Compartment* c = currentCompartment();
    return c && c->app ? *c->app : engine::currentApp();
}

uint64_t currentCompartmentId() { return currentCompartment()->id; }

Compartment* compartmentForDocument(const dom::Document* doc) {
    if (!doc) return &appSlot();
    auto it = s_byDocument.find(doc);
    if (it != s_byDocument.end()) return it->second;
    if (s_deadDocuments.count(doc)) return &deadSlot();
    return &appSlot();
}

void noteDocumentOwner(const dom::Document* doc, Compartment* owner) {
    if (!doc || !owner || owner->kind == CompartmentKind::App) return;
    s_byDocument[doc] = owner;
}

void forgetDocumentOwner(const dom::Document* doc) {
    auto it = s_byDocument.find(doc);
    if (it != s_byDocument.end() && it->second->document != doc) s_byDocument.erase(it);
}

Compartment* compartmentById(uint64_t id) {
    if (id == 0) return &appSlot();
    for (auto& c : s_live)
        if (c->id == id) return c.get();
    return nullptr;
}

Compartment* compartmentForWindowHost(uint64_t windowHostId) {
    if (windowHostId == 0) return nullptr;
    for (auto& c : s_live)
        if (c->windowHostId == windowHostId) return c.get();
    return nullptr;
}

bool sameApp(const Compartment* a, const Compartment* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    return ev::sameRealmGroup(a->realm, b->realm);
}

Compartment* compartmentForRealm(const ev::Realm* realm) {
    auto it = s_byRealm.find(realm);
    return it != s_byRealm.end() ? it->second : nullptr;
}

std::vector<Compartment*> allCompartments() {
    std::vector<Compartment*> out;
    out.push_back(&appSlot());
    for (auto& c : s_live) out.push_back(c.get());
    return out;
}

Compartment* createCompartment(engine::Engine& engine, CompartmentSetup setup) {
    auto owned = std::make_unique<Compartment>();
    Compartment* c = owned.get();
    c->id = s_nextId++;
    c->kind = setup.kind;
    c->document = setup.document;
    c->parent = setup.parent;
    c->hostElement = setup.hostElement;
    c->windowHostId = setup.windowHostId;
    c->name = setup.name.empty() ? appSlot().name : setup.name;
    c->appDir = std::move(setup.appDir);
    c->basePath = std::move(setup.basePath);
    if (setup.permissions) {
        c->ownPermissions = std::move(setup.permissions);
        c->permissions = c->ownPermissions.get();
    } else {
        c->permissions = (setup.parent ? setup.parent : &appSlot())->permissions;
    }
    c->app = setup.app ? std::move(setup.app) : (setup.parent ? setup.parent->app : nullptr);
    c->realm = ev::createRealm();
    // One app's compartments are one ownership group, joined before the realm
    // makes anything: an appview starts an app of its own, and what it hosts
    // joins it.
    if (c->kind != CompartmentKind::AppView) {
        const Compartment* owner = c->parent ? c->parent : &appSlot();
        if (owner->realm) ev::joinRealmGroup(c->realm, owner->realm);
    }
    // Another app runs in time slices on a fiber of its own, so it cannot
    // stall its host page. Its scripts compile on that stack, hence its size.
    if (c->kind == CompartmentKind::AppView) {
        ev::CompartmentOptions opts;
        opts.sliceMs = kAppViewSliceMs;
        opts.stackBytes = size_t{8} << 20;
        opts.name = c->name;
        c->fiber = ev::createCompartment(c->realm, opts);
        if (!c->fiber) LOG_WARN("appview '%s': no fiber; it runs on the page's stack", c->name.c_str());
    }
    s_live.push_back(std::move(owned));
    if (c->document) s_byDocument[c->document] = c;
    s_byRealm[c->realm] = c;
    {
        CompartmentScope scope(c);
        installRealmHostGlobals(engine, *c);
    }
    LOG_INFO("compartment %llu: %s '%s' (%s)", static_cast<unsigned long long>(c->id),
             compartmentKindName(c->kind), c->name.c_str(), c->basePath.c_str());
    return c;
}

void destroyCompartment(Compartment* c, const std::string& why) {
    if (!c || c->kind == CompartmentKind::App || c->dying) return;
    auto it = std::find_if(s_live.begin(), s_live.end(), [c](const auto& p) { return p.get() == c; });
    if (it == s_live.end()) return;
    {
        // Inside the realm, as its own code would: each closes a resource the
        // compartment's code opened and reports it there.
        CompartmentScope scope(c);
        bronze::embed::revokeHeldAccessInRealm(c->realm, (c->name.empty() ? std::string("the app") : c->name) +
                                                             " was stopped: " + why);
        for (auto h = c->teardownHooks.rbegin(); h != c->teardownHooks.rend(); ++h)
            if (*h) (*h)();
        c->teardownHooks.clear();
    }
    c->dying = true;
    // The host's tables keyed by compartment.
    clearWindowName(c->id);
    clearParsedDocumentsForCompartment(c->id);
    clearWindowProxies(c->id);
    clearCustomElementsForCompartment(c->id);
    dropObserverState(c->id);
    dropModuleGroups(c->id);
    for (PerCompartment* slot : perCompartmentSlots()) slot->drop(c->id);
    for (auto d = s_byDocument.begin(); d != s_byDocument.end();)
        d = d->second == c ? s_byDocument.erase(d) : std::next(d);
    if (c->document) s_deadDocuments.insert(c->document);
    c->document = nullptr;
    c->hostElement = nullptr;
    // Its children (a frame inside an appview) go with it.
    std::vector<Compartment*> children;
    for (auto& other : s_live)
        if (other->parent == c && !other->dying) children.push_back(other.get());
    std::unique_ptr<Compartment> owned = std::move(*it);
    s_live.erase(it);
    for (Compartment* child : children) child->parent = nullptr;
    if (compartmentInUse(c)) {
        s_dead.push_back(std::move(owned));
        return;
    }
    finishDestroy(std::move(owned));
}

void destroyCompartmentForDocument(const dom::Document* doc) {
    auto it = s_byDocument.find(doc);
    if (it != s_byDocument.end()) destroyCompartment(it->second);
    // The document itself goes now: a later one at its address is new.
    s_deadDocuments.erase(doc);
}

void destroyChildCompartments(const std::string& why) {
    std::vector<Compartment*> all;
    for (auto& c : s_live) all.push_back(c.get());
    for (Compartment* c : all) destroyCompartment(c, why);
    reapDeadCompartments();
}

void reapDeadCompartments() {
    for (auto it = s_dead.begin(); it != s_dead.end();) {
        if (compartmentInUse(it->get())) {
            ++it;
            continue;
        }
        std::unique_ptr<Compartment> owned = std::move(*it);
        it = s_dead.erase(it);
        finishDestroy(std::move(owned));
    }
}

CompartmentScope::CompartmentScope(Compartment* c) {
    if (!c) c = &appSlot();
    if (c->dying || !c->realm) return;
    ok_ = true;
    if (ev::currentRealm() == c->realm) return;
    ev::enterRealm(c->realm);
    s_entered.push_back(c->realm);
    pushed_ = true;
    // Entering another compartment from an app's fiber (a capability call
    // into the page, a clone): nothing suspends until the host is back out,
    // so the scope stack never holds a suspended fiber's entry.
    if (ev::runningCompartment() != nullptr) {
        ev::beginNoYield();
        noYield_ = true;
    }
}

CompartmentScope::~CompartmentScope() {
    if (!pushed_) return;
    if (noYield_) ev::endNoYield();
    s_entered.pop_back();
    ev::exitRealm();
}

// ---- time slices ----------------------------------------------------------

namespace {

double steadyMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// An app the runtime terminated (its sync entry ran over, or the fiber ended
// on a host error): reported, its page told, and torn down.
void stopTerminated(Compartment* c, const std::string& reason, const std::string& site) {
    if (c->dying) return;
    LOG_ERROR("%s '%s' was stopped: %s%s%s", compartmentKindName(c->kind), c->name.c_str(), reason.c_str(),
              site.empty() ? "" : "\n    at ", site.c_str());
    noteAppViewTerminated(*c, reason, site);
    destroyCompartment(c, reason);
}

void settle(Compartment* c, const ev::CompartmentStatus& s) {
    if (!s.uncaught.empty())
        LOG_ERROR("%s '%s': uncaught %s", compartmentKindName(c->kind), c->name.c_str(), s.uncaught.c_str());
    if (s.state == ev::CompartmentState::Terminated) stopTerminated(c, s.terminationReason, s.terminationSite);
}

// Run (or resume) on the fiber, from the host's own stack, entered there too
// so the host's tables see the compartment in use for the length of it.
ev::CompartmentStatus onFiber(Compartment* c, const std::function<void()>* body) {
    CompartmentScope scope(c);
    ++c->fiberDepth;
    ev::CompartmentStatus s = body ? ev::runInCompartment(c->fiber, *body) : ev::resumeCompartment(c->fiber);
    --c->fiberDepth;
    return s;
}

bool suspended(const Compartment* c) {
    return c->fiber && ev::compartmentState(c->fiber) == ev::CompartmentState::Yielded;
}

// The live compartment `id`, or null once it is stopped: settle may destroy
// one outright, so nothing reads a compartment past it but through this.
Compartment* live(uint64_t id) {
    Compartment* c = compartmentById(id);
    return c && !c->dying ? c : nullptr;
}

// Finish the job suspended on compartment `id`'s fiber by `deadlineMs`, or
// stop the app. False when it was stopped.
bool finishSuspended(uint64_t id, double deadlineMs) {
    for (Compartment* c = live(id); c && suspended(c); c = live(id)) {
        if (steadyMs() >= deadlineMs) {
            const std::string reason = "it did not return within " +
                                       std::to_string(static_cast<int>(kSyncEntryBudgetMs)) +
                                       " ms where its host page waits on it (an event or a call)";
            {
                CompartmentScope scope(c);
                ++c->fiberDepth;
                ev::terminateCompartment(c->fiber, reason);
                --c->fiberDepth;
            }
            // The termination's report, with where the app was.
            settle(c, onFiber(c, nullptr));
            if (Compartment* still = live(id)) stopTerminated(still, reason, {});
            return false;
        }
        settle(c, onFiber(c, nullptr));
    }
    return live(id) != nullptr;
}

}  // namespace

bool runEntry(Compartment* c, EntryKind kind, std::function<void()> body) {
    if (!c) c = &appSlot();
    if (c->dying || !c->realm) return false;
    // No fiber, or already on one (this app's own code reached the host, or
    // another app's called across): here, to its end.
    if (!c->fiber || !onMainThread() || ev::runningCompartment() != nullptr) {
        CompartmentScope scope(c);
        if (!scope.entered()) return false;
        body();
        return true;
    }
    if (kind == EntryKind::Async && (suspended(c) || !c->queued.empty())) {
        c->queued.push_back(std::move(body));
        return true;
    }
    const uint64_t id = c->id;
    const double deadline = steadyMs() + kSyncEntryBudgetMs;
    if (suspended(c) && !finishSuspended(id, deadline)) return false;
    settle(c, onFiber(c, &body));
    if (kind == EntryKind::Sync) {
        if (Compartment* still = live(id); still && suspended(still)) finishSuspended(id, deadline);
    }
    return true;
}

bool runEntry(const dom::Document* doc, EntryKind kind, std::function<void()> body) {
    return runEntry(compartmentForDocument(doc), kind, std::move(body));
}

void serviceCompartmentFibers() {
    std::vector<uint64_t> ids;
    for (auto& c : s_live)
        if (c->fiber && !c->dying) ids.push_back(c->id);
    for (uint64_t id : ids) {
        // Each step may tear any compartment down: looked up again.
        Compartment* c = compartmentById(id);
        if (!c || c->dying || !c->fiber) continue;
        if (suspended(c)) settle(c, onFiber(c, nullptr));
        while ((c = compartmentById(id)) && !c->dying && !suspended(c) && !c->queued.empty()) {
            std::function<void()> body = std::move(c->queued.front());
            c->queued.pop_front();
            settle(c, onFiber(c, &body));
        }
    }
    dispatchAppViewTerminations();
}

namespace {
std::vector<PerCompartment*>& perCompartmentSlots() {
    static auto* v = new std::vector<PerCompartment*>();
    return *v;
}
}  // namespace

PerCompartment::PerCompartment() { perCompartmentSlots().push_back(this); }

PerCompartment::~PerCompartment() {
    auto& v = perCompartmentSlots();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

void PerCompartment::set(Value v) {
    ev::Persistent value(v);
    const uint64_t id = currentCompartment()->id;
    for (auto& [cid, slot] : slots_)
        if (cid == id) {
            slot->set(value.get());
            return;
        }
    slots_.emplace_back(id, std::make_unique<ev::Persistent>(value.get()));
}

Value PerCompartment::get() const { return getFor(currentCompartment()->id); }

Value PerCompartment::getFor(uint64_t compartmentId) const {
    for (const auto& [cid, slot] : slots_)
        if (cid == compartmentId) return slot->get();
    return ev::undefined();
}

void PerCompartment::forEachFunction(const std::function<void(Value fn)>& call) const {
    std::vector<std::pair<uint64_t, ev::Persistent>> fns;
    for (const auto& [cid, slot] : slots_)
        if (ev::isFunction(slot->get())) fns.emplace_back(cid, ev::Persistent(slot->get()));
    for (auto& [cid, fn] : fns) {
        Compartment* c = compartmentById(cid);
        if (!c) continue;
        CompartmentScope scope(c);
        if (scope.entered()) call(fn.get());
    }
}

void PerCompartment::drop(uint64_t compartmentId) {
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [compartmentId](const auto& s) { return s.first == compartmentId; }),
                 slots_.end());
}

// ---- promise jobs (bronze's job-context hooks) ------------------------------
// A promise job runs in the compartment that subscribed it (a `then`, an
// `await` in a frame's script), as NewPromiseReactionJob runs in its handler's
// realm. The checkpoint itself runs wherever the host drains it, so each job
// carries its compartment (id + 1; 0 is "none") and enters it.

namespace {

std::vector<bool> s_jobEntered;

uint64_t captureJobContext() {
    if (!onMainThread()) return 0;
    return currentCompartmentId() + 1;
}

void enterJobContext(uint64_t context) {
    bool pushed = false;
    if (context != 0 && onMainThread()) {
        Compartment* c = compartmentById(context - 1);
        // A compartment gone since has no realm to enter: the job runs where
        // it is, and bronze refuses its dead realm's functions.
        if (c && !c->dying && c->realm && ev::currentRealm() != c->realm) {
            ev::enterRealm(c->realm);
            s_entered.push_back(c->realm);
            pushed = true;
        }
    }
    s_jobEntered.push_back(pushed);
}

void leaveJobContext(uint64_t) {
    if (s_jobEntered.empty()) return;
    const bool pushed = s_jobEntered.back();
    s_jobEntered.pop_back();
    if (pushed) {
        s_entered.pop_back();
        ev::exitRealm();
    }
}

}  // namespace

void installJobCompartmentHooks() {
    ev::setJobContextHooks(&captureJobContext, &enterJobContext, &leaveJobContext);
}

bool firstInRealm(const void* key) {
    return t_realmKeys[ev::currentRealm()].insert(key).second;
}

std::string compartmentLogPrefix() {
    Compartment* c = currentCompartment();
    if (!c || c->kind == CompartmentKind::App) return {};
    return std::string("[") + compartmentKindName(c->kind) + " " + c->name + "] ";
}

}  // namespace bro::bronze_host

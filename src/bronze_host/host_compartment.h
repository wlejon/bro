#pragma once

// Compartments (docs/app-isolation.md, step 3): one app's code, contained.
//
// A compartment is a real bronze Realm (its own global object, its own host
// globals and host classes, its own module instances) bound to one document
// and one AppPermissions. The app's document is the app compartment, on the
// thread's default realm. Every sub-document the engine hosts is a compartment
// of its own: an <iframe>, a secondary window (bro.window.open / window.open),
// a system panel, and an <appview> (an app running inline in its host page,
// with its own manifest identity and grants).
//
// What follows from a compartment being a realm:
//   - A function runs in the realm that made it, whoever calls it (bronze
//     enters the callee's realm on every call), so its free names are its own
//     globals.
//   - A host object (an element, a canvas, a scene handle, any HostClass
//     instance) names the realm that made it, and unwraps only in a realm of
//     that realm's ownership group. The app's document, its frames, windows
//     and panels are one group (sameApp): they reach one another's windows and
//     documents as the web's same-origin frames do. An appview is a group of
//     its own: a DOM node of the page passed into it is refused beside the
//     brand check, and the other way.
//   - Between apps nothing mutable crosses. An appview talks to its host page
//     over postMessage (structured clone, bytes in between) and through
//     capability objects the page grants (host_membrane.h): arguments cloned
//     in, results cloned out, callbacks made capabilities in turn.
//
// The current compartment is the current realm's, so the current document is
// the current compartment's (currentHostDocument). Host code that calls into a
// compartment from outside any of its calls (a timer, a listener, an rAF, a
// message) enters it with a CompartmentScope around building the arguments and
// making the call, so what it makes (an Event, a MessageEvent) is that realm's.
//
// TIME SLICES (step 4): an appview runs on a fiber of its own (bronze's
// embed::createCompartment), so no app can stall its host page. The entries
// that start its code (a timer, a frame callback, a message, its scripts, an
// event) go through runEntry, which runs them on the fiber. One that outlasts
// its slice yields: an async entry finishes on later frames
// (serviceCompartmentFibers), and a sync one, whose caller needs it done,
// finishes within a budget or the app is stopped with the reason. Host code
// that enters ANOTHER compartment from a fiber (a capability call into the
// page, a clone) holds yields off for the length of its CompartmentScope, so
// nothing suspends while the host is mid-call into someone else's realm.

#include "bronze_host/host_class.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::dom { class Document; class Element; }
namespace bro::engine { class AppPermissions; class Engine; struct AppRuntimeInfo; }

namespace bro::bronze_host {

enum class CompartmentKind : uint8_t {
    App,      // the app's own document, on the default realm
    Frame,    // an <iframe src=dir>: the app's grants
    Window,   // a secondary window: the app's grants
    Panel,    // a system panel (bro's own UI): the app's grants
    AppView,  // <appview src=dir>: its own manifest, identity and grants
};

const char* compartmentKindName(CompartmentKind kind);

struct Compartment {
    uint64_t id = 0;  // 0 is the app
    CompartmentKind kind = CompartmentKind::App;
    ev::Realm* realm = nullptr;
    // Its document; the app's follows the engine's (null here).
    dom::Document* document = nullptr;
    // The compartment that made it (its host page), and the element in that
    // page it is shown in (the <iframe>, the <appview>); null for the app, a
    // window and a panel.
    Compartment* parent = nullptr;
    dom::Element* hostElement = nullptr;
    // A secondary window's engine id (WindowHost::id), 0 otherwise.
    uint64_t windowHostId = 0;
    // Who it is: the app's name, or the appview's manifest name; what logs
    // and refusals name.
    std::string name;
    std::string appDir, basePath;
    // Never null. Frames, windows and panels share the app's; an appview owns
    // its own (ownPermissions).
    engine::AppPermissions* permissions = nullptr;
    std::unique_ptr<engine::AppPermissions> ownPermissions;
    // An appview's identity as bro.app shows it (its id, manifest, folders);
    // null shares the process's (engine::currentApp()).
    std::shared_ptr<const engine::AppRuntimeInfo> app;
    // Torn down: its realm is destroyed at the next frame seam (never while
    // its own code is on the stack), and nothing enters it meanwhile.
    bool dying = false;
    // Run at teardown, last registered first, inside the realm: what the
    // compartment's own natives created and must release (step 5's ledger
    // extends this).
    std::vector<std::function<void()>> teardownHooks;
    // An appview's fiber: its entries run there in time slices (runEntry).
    // Null runs them on the caller's stack.
    ev::Compartment* fiber = nullptr;
    // Entries of its running on the fiber now (a nested one too): its realm
    // is not destroyed while one is.
    int fiberDepth = 0;
    // Async entries that arrived while a job was suspended on the fiber, run
    // in order once it finishes.
    std::deque<std::function<void()>> queued;
    // When the job suspended on the fiber first yielded (steady ms, 0 when
    // none is), and whether it has run past kNotRespondingMs.
    double suspendedSinceMs = 0;
    bool notResponding = false;
};

/// The app's compartment (the default realm, the engine's document).
Compartment& appCompartment();
/// The compartment the running code belongs to: the current realm's. Never
/// null; the app's when the realm is no compartment's (a Worker's thread).
Compartment* currentCompartment();
bool inChildCompartment();
/// What host tables keyed per compartment (window names, custom-element
/// registries, DOMParser documents) use: 0 for the app.
uint64_t currentCompartmentId();
/// The compartment that owns `doc`: the app's for the engine's document, or
/// for a document no compartment was made for (a DOMParser result is its
/// parser's).
Compartment* compartmentForDocument(const dom::Document* doc);
Compartment* compartmentById(uint64_t id);
/// A document a compartment's code made (DOMParser, createHTMLDocument) is
/// that compartment's; forgotten when the document goes.
void noteDocumentOwner(const dom::Document* doc, Compartment* owner);
void forgetDocumentOwner(const dom::Document* doc);
Compartment* compartmentForWindowHost(uint64_t windowHostId);
Compartment* compartmentForRealm(const ev::Realm* realm);
/// Every live compartment, the app's first.
std::vector<Compartment*> allCompartments();
/// Whether `a` and `b` are one app: the app's document, its frames, windows
/// and panels are (one realm ownership group, so each reaches the others'
/// windows and documents as the web's same-origin frames do); an appview
/// and what it hosts are an app of their own.
bool sameApp(const Compartment* a, const Compartment* b);

struct CompartmentSetup {
    CompartmentKind kind = CompartmentKind::Frame;
    dom::Document* document = nullptr;
    Compartment* parent = nullptr;
    dom::Element* hostElement = nullptr;
    uint64_t windowHostId = 0;
    std::string name, appDir, basePath;
    // An appview's own grants and identity; null shares the app's.
    std::unique_ptr<engine::AppPermissions> permissions;
    std::shared_ptr<const engine::AppRuntimeInfo> app;
};

/// The identity of the running code's app: an appview's own, else the
/// process's (engine::currentApp()).
const engine::AppRuntimeInfo& compartmentApp();

/// Make a compartment: a fresh realm, the host's globals installed into it
/// (the document's own `document`, `window`, timers, the `bro` root from its
/// grants, ...), registered for `setup.document`.
Compartment* createCompartment(engine::Engine& engine, CompartmentSetup setup);

/// Tear `c` down: what it holds open is revoked (with the refusal naming it),
/// its teardown hooks run, and its realm goes. Never the app's.
void destroyCompartment(Compartment* c, const std::string& why = "it was closed");
void destroyCompartmentForDocument(const dom::Document* doc);
/// Every compartment but the app's (a reload).
void destroyChildCompartments(const std::string& why);
/// Destroy the realms of compartments torn down while their code ran: the
/// frame seam calls this first.
void reapDeadCompartments();

/// Enter `c` (its realm) for the length of a host call into it. A dying
/// compartment is not entered: `entered()` answers false and the caller skips
/// the call.
class CompartmentScope {
public:
    explicit CompartmentScope(Compartment* c);
    explicit CompartmentScope(const dom::Document* doc) : CompartmentScope(compartmentForDocument(doc)) {}
    ~CompartmentScope();
    CompartmentScope(const CompartmentScope&) = delete;
    CompartmentScope& operator=(const CompartmentScope&) = delete;
    bool entered() const { return ok_; }

private:
    bool pushed_ = false;
    bool noYield_ = false;
    bool ok_ = false;
};

/// An appview's slice: how long one entry, or one resumption of it, runs
/// before it yields to the host.
inline constexpr double kAppViewSliceMs = 8.0;
/// How long the host waits on a sync entry (with what was suspended before
/// it) before it stops the app.
inline constexpr double kSyncEntryBudgetMs = 1000.0;
/// How long one job of an appview's may run, yielding slice by slice, before
/// its page hears the app is not responding (its `unresponsive` event).
inline constexpr double kNotRespondingMs = 5000.0;

/// How the host waits on an entry into a compartment's code.
enum class EntryKind : uint8_t {
    // The caller goes on without its end (a timer, a frame callback, a
    // message, a document's scripts). In an app with a fiber it may yield at
    // its slice and finish on later frames, and one that arrives while
    // another is suspended runs after it.
    Async,
    // The caller needs it done (an event whose defaultPrevented it reads, a
    // call whose result it returns). A job suspended in the app is finished
    // first, and the entry runs to its end; an app that does not finish
    // within kSyncEntryBudgetMs is stopped, with the reason.
    Sync,
};

/// Run `body` as an entry into `c`'s code, `c` entered: on its fiber when it
/// has one, else on this stack. An async body may run later, so it owns what
/// it captures. False when `c` is torn down (nothing ran).
bool runEntry(Compartment* c, EntryKind kind, std::function<void()> body);
bool runEntry(const dom::Document* doc, EntryKind kind, std::function<void()> body);

/// The frame seam: each app suspended at its slice runs one more slice, and
/// one that finished runs the entries queued meanwhile.
void serviceCompartmentFibers();

/// One JS value per compartment, where the host once kept one per process: a
/// dispatcher each compartment's bro_core.js registers, a per-document
/// singleton (document.timeline). `set`/`get` act on the current
/// compartment's; a compartment's entry is dropped at its teardown.
class PerCompartment {
public:
    PerCompartment();
    ~PerCompartment();
    PerCompartment(const PerCompartment&) = delete;
    PerCompartment& operator=(const PerCompartment&) = delete;

    void set(Value v);
    Value get() const;                 // undefined when it has none
    Value getFor(uint64_t compartmentId) const;
    /// Each compartment holding a function, entered, with its function: the
    /// caller makes the call inside.
    void forEachFunction(const std::function<void(Value fn)>& call) const;
    void drop(uint64_t compartmentId);

private:
    std::vector<std::pair<uint64_t, std::unique_ptr<ev::Persistent>>> slots_;
};

/// True the first time it is asked in the current realm for `key`: an
/// installer that must run once per realm (its class, its prototype) guards
/// on it.
bool firstInRealm(const void* key);

/// The current compartment's name for a log line ("" for the app).
std::string compartmentLogPrefix();

/// Promise jobs run in the compartment that subscribed them.
void installJobCompartmentHooks();

/// Make the app's compartment: the current (default) realm and the engine's
/// document. installWebHostGlobals calls it before anything else.
void initAppCompartment(engine::Engine& engine);
/// The host's per-realm globals, installed into the current realm for `c`
/// (dom_globals.cpp): everything a document's code sees that is its own.
void installRealmHostGlobals(engine::Engine& engine, Compartment& c);

}  // namespace bro::bronze_host

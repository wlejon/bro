// <appview src="appdir"> (host_appview.h): an app's identity and grants for its
// compartment, its prompts routed to the host page, its lifecycle and the
// capabilities its page exposes.

#include "bronze_host/host_appview.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_iframe.h"
#include "bronze_host/host_membrane.h"
#include "bronze_host/host_runtime.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/shadow_root.h"
#include "engine/app_manifest.h"
#include "engine/app_permissions.h"
#include "engine/app_runtime.h"
#include "engine/engine.h"
#include "engine/settings.h"
#include "util/log.h"
#include "util/model_dirs.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace fs = std::filesystem;
using AskAnswer = engine::AppPermissions::AskAnswer;

// What a page exposed to an appview, by element: kept across stop / start /
// reload, so each run of the app sees them.
struct Exposed {
    std::string name;
    uint64_t owner = 0;
    std::unique_ptr<ev::Persistent> fn;
};
std::unordered_map<const dom::Element*, std::vector<Exposed>> s_exposed;
// Documents whose freed nodes clear s_exposed, so a later element at a freed
// one's address never inherits what was exposed to it.
std::unordered_set<const dom::Document*> s_exposedDocs;
// The views showing an error card, so a new run of the app clears only a
// card bro put there.
std::unordered_set<const dom::Element*> s_cardViews;

void onExposedNodeFreed(dom::Document*, dom::Node* node) {
    s_exposed.erase(static_cast<const dom::Element*>(node));
    s_cardViews.erase(static_cast<const dom::Element*>(node));
}

Compartment* viewCompartment(dom::Element* el) {
    engine::Engine* eng = hostEngine();
    if (!el || !eng) return nullptr;
    engine::IframeDoc* d = eng->iframeDocForElement(el);
    if (!d || !d->document) return nullptr;
    Compartment* c = compartmentForDocument(d->document.get());
    return c && c->kind == CompartmentKind::AppView && c->document == d->document.get() && !c->dying ? c : nullptr;
}

Value stringArray(const std::vector<std::string>& v) {
    return hostArrayOf(v.size(), [&v](size_t i) { return ev::fromUtf8(v[i]); });
}

// The host page's answer to a prompt of compartment `id`: a
// `permissionrequest` event at its element, in the page.
AskAnswer askHostPage(uint64_t id, std::string_view capIn, const std::string& question, const std::string& what) {
    Compartment* c = compartmentById(id);
    if (!c || !c->parent || !c->hostElement) return AskAnswer::Unhandled;
    const std::string cap(capIn);
    // Answered during the dispatch: the event's answer. After it: kept as
    // the user's decision, for a page that asked in its own time.
    struct State {
        bool dispatching = true;
        AskAnswer answer = AskAnswer::Unhandled;
    };
    auto state = std::make_shared<State>();
    auto answerFn = [state, id, cap](bool allow) {
        return [state, id, cap, allow](Value, std::span<const Value>) -> Value {
            if (state->dispatching) {
                state->answer = allow ? AskAnswer::Allow : AskAnswer::Deny;
            } else if (Compartment* v = compartmentById(id); v && v->permissions) {
                v->permissions->decide(cap, allow);
            }
            return ev::undefined();
        };
    };

    CompartmentScope scope(c->parent);
    if (!scope.entered()) return AskAnswer::Unhandled;
    ev::Persistent el(hostElementValue(c->hostElement));
    ev::Persistent ctor(ev::globalValue("CustomEvent").value);
    if (!ev::isObject(el.get()) || !ev::isFunction(ctor.get())) return AskAnswer::Unhandled;
    ev::Persistent detail(ev::createObject());
    ev::setProperty(detail.get(), "capability", ev::fromUtf8(cap));
    ev::setProperty(detail.get(), "question", ev::fromUtf8(question));
    ev::setProperty(detail.get(), "what", ev::fromUtf8(what));
    ev::setProperty(detail.get(), "app", ev::fromUtf8(c->name));
    ev::Persistent init(ev::createObject());
    ev::setProperty(init.get(), "detail", detail.get());
    ev::setProperty(init.get(), "cancelable", ev::fromBool(true));
    ev::Persistent type(ev::fromUtf8("permissionrequest"));
    Value cargs[2] = {type.get(), init.get()};
    ev::CallResult made = ev::construct(ctor.get(), std::span<const Value>(cargs, 2));
    if (made.thrown) {
        reportBronzeError("appview permissionrequest", made.value);
        return AskAnswer::Unhandled;
    }
    ev::Persistent evt(made.value);
    {
        ev::Persistent allow(ev::makeFunction(answerFn(true), 0));
        ev::setProperty(evt.get(), "allow", allow.get());
        ev::Persistent deny(ev::makeFunction(answerFn(false), 0));
        ev::setProperty(evt.get(), "deny", deny.get());
    }
    ev::Persistent dispatch(ev::getProperty(el.get(), "dispatchEvent"));
    if (!ev::isFunction(dispatch.get())) return AskAnswer::Unhandled;
    Value arg = evt.get();
    ev::CallResult r = ev::call(dispatch.get(), el.get(), std::span<const Value>(&arg, 1));
    state->dispatching = false;
    if (r.thrown) reportBronzeError("appview permissionrequest", r.value);
    if (state->answer != AskAnswer::Unhandled) return state->answer;
    Value prevented = ev::getProperty(evt.get(), "defaultPrevented");
    return ev::isBool(prevented) && ev::toBool(prevented) ? AskAnswer::Pending : AskAnswer::Unhandled;
}

// What the runtime tells a page about its app, at the next frame seam: the
// note may come from deep in the host's own stack, or from inside the app.
struct ViewEvent {
    uint64_t page = 0;
    std::unique_ptr<ev::Persistent> element;
    std::string type;  // terminated, apperror, unresponsive, responsive
    std::string app;
    std::string text;  // terminated: the reason; apperror: the error
    std::string site;
};
std::vector<ViewEvent> s_viewEvents;

void queueViewEvent(Compartment& c, const char* type, std::string text, std::string site) {
    if (c.kind != CompartmentKind::AppView || !c.parent || !c.hostElement) return;
    CompartmentScope scope(c.parent);
    if (!scope.entered()) return;
    s_viewEvents.push_back({c.parent->id, std::make_unique<ev::Persistent>(hostElementValue(c.hostElement)), type,
                            c.name, std::move(text), std::move(site)});
}

const char* const kCardCss =
    ":host { display: block; }"
    ".card { box-sizing: border-box; width: 100%; height: 100%; padding: 16px;"
    " background: #2a1d1f; color: #f3e6e6; font: 13px system-ui, sans-serif; overflow: hidden; }"
    ".title { font-weight: 600; font-size: 15px; margin-bottom: 8px; }"
    ".reason { margin-bottom: 8px; }"
    ".site { font-family: monospace; font-size: 11px; color: #c9a9ab; white-space: pre-wrap; }";

void removeChildren(dom::Node* n) {
    while (!n->childNodes().empty()) n->removeChild(n->childNodes().back());
}

// The stopped view's error card: who stopped, why and where, drawn in a
// closed shadow root of the element (a view with no document paints its own
// content), until the app runs again.
void showErrorCard(dom::Element* el, const ViewEvent& t) {
    dom::Document* doc = el->document();
    if (!doc) return;
    dom::ShadowRoot* root = el->shadowRoot();
    if (!root) {
        root = el->attachShadow(dom::ShadowRoot::Mode::Closed);
        if (!root) return;
    } else if (!s_cardViews.count(el)) {
        return;  // the page's own shadow root: not bro's to fill
    }
    removeChildren(root);
    auto add = [&](dom::Node* parent, const char* tag, const char* cls, const std::string& text) {
        dom::Element* e = doc->createElement(tag);
        if (cls) e->setAttribute("class", cls);
        if (!text.empty()) e->setTextContent(text);
        parent->appendChild(e);
        return e;
    };
    add(root, "style", nullptr, kCardCss);
    dom::Element* card = add(root, "div", "card", {});
    add(card, "div", "title", (t.app.empty() ? std::string("The app") : t.app) + " was stopped");
    add(card, "div", "reason", t.text);
    if (!t.site.empty()) add(card, "div", "site", t.site);
    s_cardViews.insert(el);
    if (s_exposedDocs.insert(doc).second) doc->addNodeFreedObserver(&onExposedNodeFreed);
    doc->markDirty();
}

void clearErrorCard(dom::Element* el) {
    if (!el || !s_cardViews.erase(el)) return;
    if (dom::ShadowRoot* root = el->shadowRoot()) removeChildren(root);
    if (dom::Document* doc = el->document()) doc->markDirty();
}

}  // namespace

void noteAppViewTerminated(Compartment& c, const std::string& reason, const std::string& site) {
    queueViewEvent(c, "terminated", reason, site);
}

void noteAppViewError(Compartment& c, const std::string& error) {
    queueViewEvent(c, "apperror", error, {});
}

void noteAppViewResponding(Compartment& c, bool responding) {
    queueViewEvent(c, responding ? "responsive" : "unresponsive", {}, {});
}

void dispatchAppViewEvents() {
    if (s_viewEvents.empty()) return;
    std::vector<ViewEvent> list = std::move(s_viewEvents);
    s_viewEvents.clear();
    for (ViewEvent& t : list) {
        Compartment* page = compartmentById(t.page);
        if (!page) continue;
        const bool terminated = t.type == "terminated";
        // The view stops as stop() stops it (its document goes), out here
        // where nothing of the engine's is mid-dispatch into it.
        if (terminated) {
            CompartmentScope scope(page);
            if (!scope.entered()) continue;
            if (dom::Element* el = hostElementOf(t.element->get()))
                if (engine::Engine* eng = hostEngine()) eng->stopIframe(el);
        }
        bool prevented = false;
        runEntry(page, EntryKind::Sync, [&t, &prevented, terminated] {
            ev::Persistent ctor(ev::globalValue("CustomEvent").value);
            if (!ev::isObject(t.element->get()) || !ev::isFunction(ctor.get())) return;
            ev::Persistent detail(ev::createObject());
            if (terminated) ev::setProperty(detail.get(), "reason", ev::fromUtf8(t.text));
            if (t.type == "apperror") ev::setProperty(detail.get(), "error", ev::fromUtf8(t.text));
            if (terminated) ev::setProperty(detail.get(), "site", ev::fromUtf8(t.site));
            ev::Persistent init(ev::createObject());
            ev::setProperty(init.get(), "detail", detail.get());
            // A page that draws its own card cancels terminated.
            ev::setProperty(init.get(), "cancelable", ev::fromBool(terminated));
            ev::Persistent type(ev::fromUtf8(t.type));
            Value cargs[2] = {type.get(), init.get()};
            ev::CallResult made = ev::construct(ctor.get(), std::span<const Value>(cargs, 2));
            if (made.thrown) {
                reportBronzeError("appview event", made.value);
                return;
            }
            ev::Persistent evt(made.value);
            ev::Persistent dispatch(ev::getProperty(t.element->get(), "dispatchEvent"));
            if (!ev::isFunction(dispatch.get())) return;
            Value arg = evt.get();
            ev::CallResult r = ev::call(dispatch.get(), t.element->get(), std::span<const Value>(&arg, 1));
            if (r.thrown) reportBronzeError("appview event", r.value);
            Value dp = ev::getProperty(evt.get(), "defaultPrevented");
            prevented = ev::isBool(dp) && ev::toBool(dp);
        });
        if (terminated && !prevented) {
            CompartmentScope scope(page);
            if (!scope.entered()) continue;
            if (dom::Element* el = hostElementOf(t.element->get())) showErrorCard(el, t);
        }
    }
}

void prepareAppView(engine::Engine& engine, CompartmentSetup& setup) {
    std::error_code ec;
    const std::string dir = fs::absolute(fs::path(setup.appDir), ec).lexically_normal().string();
    engine::AppDescriptor d;
    const fs::path manifest = fs::path(dir) / "bro.json";
    if (fs::is_regular_file(manifest, ec)) {
        std::string err;
        if (!engine::parseAppManifest(manifest.string(), d, &err))
            LOG_WARN("appview %s: %s; it runs with no identity of its own and no permissions", dir.c_str(),
                     err.c_str());
    }
    auto info = std::make_shared<engine::AppRuntimeInfo>();
    info->id = engine::appIdFor(d, dir);
    info->dir = dir;
    info->manifest = d;
    info->cwd = engine::currentWorkingDirectory();
    info->dirs = engine::appDirsFor(info->id);

    engine::PermissionsSetup perms;
    perms.appDir = dir;
    perms.appId = info->id;
    perms.appName = d.name;
    perms.declared = d.permissions;
    // The host page's own grants never pass to the app it runs: what the
    // host executable granted was granted to the page.
    perms.dirs = info->dirs;
    // bro's shared asset folders (/system, /std) are readable by every app;
    // the page's own folder and the mounts inside it (/app) are the page's.
    const std::string hostDir = engine::canonicalPath(engine.appDir());
    for (const auto& [prefix, mount] : engine.assetMounts().mounts()) {
        const std::string m = engine::canonicalPath(mount);
        const bool inHost = !hostDir.empty() && (m == hostDir || m.rfind(hostDir + "/", 0) == 0);
        if (!inHost) perms.readRoots.push_back(mount);
    }
    if (engine::Settings* s = engine.settings())
        for (const std::string& m : util::modelDirs(s->getString("models.dirs"))) perms.readRoots.push_back(m);
    auto p = std::make_unique<engine::AppPermissions>();
    p->configure(perms);

    setup.name = d.name.empty() ? info->id : d.name;
    setup.permissions = std::move(p);
    setup.app = std::move(info);
}

void attachAppViewCompartment(Compartment& c) {
    // A new run of the app: its view's error card, if it had one, goes.
    clearErrorCard(c.hostElement);
    {
        ev::GlobalValue broG = ev::globalValue("bro");
        ev::Persistent bro(broG.found ? broG.value : ev::undefined());
        ev::Persistent app(ev::isObject(bro.get()) ? ev::getProperty(bro.get(), "app") : ev::undefined());
        installHostCapabilities(c, app.get());
    }
    if (auto it = s_exposed.find(c.hostElement); it != s_exposed.end())
        for (Exposed& e : it->second)
            if (Compartment* owner = compartmentById(e.owner))
                exposeCapability(owner, &c, e.name, e.fn->get());
    if (c.permissions) {
        const uint64_t id = c.id;
        c.permissions->setAsker([id](std::string_view cap, const std::string& question, const std::string& what) {
            return askHostPage(id, cap, question, what);
        });
    }
}

void decorateAppViewProto(ObjectBuilder& b) {
    // src, width, height, contentWindow, contentDocument (null), reload(),
    // capture(): an <iframe>'s.
    decorateIFrameProto(b);

    b.def("start", 0, [](Value self, std::span<const Value>) -> Value {
        if (dom::Element* el = hostElementOf(self))
            if (engine::Engine* eng = hostEngine()) eng->startIframe(el);
        return ev::undefined();
    });
    b.def("stop", 0, [](Value self, std::span<const Value>) -> Value {
        if (dom::Element* el = hostElementOf(self))
            if (engine::Engine* eng = hostEngine()) eng->stopIframe(el);
        return ev::undefined();
    });
    b.accessor("running",
               [](Value self, std::span<const Value>) -> Value {
                   return ev::fromBool(viewCompartment(hostElementOf(self)) != nullptr);
               },
               nullptr);
    // False while a job of the app's has run for kNotRespondingMs without
    // finishing (it yields, so the page goes on); null when not running.
    b.accessor("responding",
               [](Value self, std::span<const Value>) -> Value {
                   Compartment* c = viewCompartment(hostElementOf(self));
                   return c ? ev::fromBool(!c->notResponding) : ev::null();
               },
               nullptr);
    b.accessor("appId",
               [](Value self, std::span<const Value>) -> Value {
                   Compartment* c = viewCompartment(hostElementOf(self));
                   return c && c->app ? ev::fromUtf8(c->app->id) : ev::null();
               },
               nullptr);
    b.accessor("appName",
               [](Value self, std::span<const Value>) -> Value {
                   Compartment* c = viewCompartment(hostElementOf(self));
                   return c ? ev::fromUtf8(c->name) : ev::null();
               },
               nullptr);
    // What the app declares and what it holds: {declared, granted, denied,
    // undecided}, or null when it is not running.
    b.accessor("permissions",
               [](Value self, std::span<const Value>) -> Value {
                   Compartment* c = viewCompartment(hostElementOf(self));
                   if (!c || !c->permissions) return ev::null();
                   const engine::PermissionsReport r = c->permissions->report();
                   ObjectBuilder o;
                   {
                       ev::Persistent v(stringArray(r.declared));
                       o.set("declared", v.get());
                   }
                   {
                       ev::Persistent v(stringArray(r.granted));
                       o.set("granted", v.get());
                   }
                   {
                       ev::Persistent v(stringArray(r.denied));
                       o.set("denied", v.get());
                   }
                   {
                       ev::Persistent v(stringArray(r.undecided));
                       o.set("undecided", v.get());
                   }
                   return o.get();
               },
               nullptr);
    // The user's answer about one of the app's capabilities, from a prompt
    // the page drew itself: kept as bro's own dialog's answer is.
    b.def("decide", 2, [](Value self, std::span<const Value> a) -> Value {
        Compartment* c = viewCompartment(hostElementOf(self));
        if (!c || !c->permissions) return ev::throwTypeError("appview.decide: the app is not running");
        if (!ev::isString(argAt(a, 0))) return ev::throwTypeError("appview.decide: a capability name is required");
        c->permissions->decide(ev::toUtf8(argAt(a, 0)), ev::toBool(argAt(a, 1)));
        return ev::undefined();
    });
    // Grant the app a function of this page's, as bro.app.host[name] inside
    // it: called across the membrane, its arguments and result cloned.
    b.def("expose", 2, [](Value self, std::span<const Value> a) -> Value {
        dom::Element* el = hostElementOf(self);
        if (!el) return ev::undefined();
        if (!ev::isString(argAt(a, 0)) || !ev::isFunction(argAt(a, 1)))
            return ev::throwTypeError("appview.expose(name, fn): a name and a function are required");
        ev::Persistent fn(argAt(a, 1));
        const std::string name = ev::toUtf8(argAt(a, 0));
        Compartment* owner = currentCompartment();
        if (dom::Document* doc = el->document(); doc && s_exposedDocs.insert(doc).second)
            doc->addNodeFreedObserver(&onExposedNodeFreed);
        auto& list = s_exposed[el];
        bool replaced = false;
        for (Exposed& e : list)
            if (e.name == name) {
                e.owner = owner->id;
                e.fn->set(fn.get());
                replaced = true;
            }
        if (!replaced) list.push_back({name, owner->id, std::make_unique<ev::Persistent>(fn.get())});
        if (Compartment* c = viewCompartment(el)) exposeCapability(owner, c, name, fn.get());
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host

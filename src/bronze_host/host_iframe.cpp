#include "bronze_host/host_iframe.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_proxy.h"
#include "bronze_host/host_document.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_window_open.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_storage.h"
#include "dom/element.h"
#include "engine/app_loader.h"
#include "engine/engine.h"
#include "util/log.h"

namespace bro::bronze_host {

namespace {

static std::unordered_map<uint64_t, ev::Persistent> s_contentWindowProxies;

// The global `document`, and globalThis.document, set to `docIn`.
void setDocumentGlobal(Value docIn) {
    ev::Persistent doc(docIn);
    ev::registerGlobal("document", doc.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "document", doc.get());
}

// The functions of a frame's realm its contentWindow handed out, each with
// the wrapper that runs it there: one wrapper a function, so a function read
// twice is the same one.
static std::unordered_map<uint64_t, std::vector<std::pair<ev::Persistent, ev::Persistent>>> s_realmFunctions;

Value wrapRealmFunction(uint64_t scopeId, dom::Document* doc, Value fnIn);

// Call `fn` of a frame's realm in that realm, as a browser runs a function
// in its own: its scope, its document as the global `document` and the
// current one, the caller's back after it returns or throws. Sub-document
// realms share one global object, entered by swapping these
// (runHostSubDocScripts, a timer, an event); a call from the parent through
// contentWindow entered nothing, so the child's function ran against the
// parent's document.
Value callInRealm(uint64_t scopeId, dom::Document* doc, Value fnIn, Value selfIn, std::span<const Value> argsIn) {
    // Rooted: entering and leaving the realm moves expandos on and off
    // globalThis, a chain of allocating calls.
    ev::Persistent fn(fnIn), self(selfIn);
    std::vector<ev::Persistent> args;
    args.reserve(argsIn.size());
    for (Value v : argsIn) args.emplace_back(v);
    dom::Document* prevDoc = currentHostDocument();
    const bool enter = doc && doc != prevDoc;
    ev::GlobalValue docG = ev::globalValue("document");
    ev::Persistent prevDocVal(docG.found ? docG.value : ev::null());
    if (enter) {
        enterRealmScope(scopeId);
        setCurrentHostDocument(doc);
        setDocumentGlobal(hostDocumentValue(doc));
    }
    std::vector<Value> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.get());
    ev::CallResult r = ev::call(fn.get(), self.get(), std::span<const Value>(argv.data(), argv.size()));
    ev::Persistent result(r.value);
    if (enter) {
        if (!ev::isNull(prevDocVal.get())) setDocumentGlobal(prevDocVal.get());
        setCurrentHostDocument(prevDoc);
        exitRealmScope();
    }
    if (r.thrown) return ev::throwValue(result.get());
    // A function it returns is of its realm too.
    if (ev::isFunction(result.get())) return wrapRealmFunction(scopeId, doc, result.get());
    return result.get();
}

Value wrapRealmFunction(uint64_t scopeId, dom::Document* doc, Value fnIn) {
    ev::Persistent fn(fnIn);
    auto& wrapped = s_realmFunctions[scopeId];
    for (auto& [f, w] : wrapped)
        if (f.get().rawBits() == fn.get().rawBits()) return w.get();
    auto held = std::make_shared<ev::Persistent>(fn.get());
    ev::Persistent wrapper(ev::makeFunction(
        [scopeId, doc, held](Value self, std::span<const Value> a) -> Value {
            return callInRealm(scopeId, doc, held->get(), self, a);
        },
        0));
    wrapped.emplace_back(ev::Persistent(fn.get()), ev::Persistent(wrapper.get()));
    return wrapped.back().second.get();
}

Value makeContentWindowProxy(dom::Element* el, engine::IframeDoc* d) {
    if (!el || !d || !d->document) return ev::null();
    uint64_t scopeId = scopeIdForDocument(d->document.get());
    auto it = s_contentWindowProxies.find(scopeId);
    if (it != s_contentWindowProxies.end()) {
        return it->second.get();
    }

    HostProxyTraps traps;
    dom::Document* docPtr = d->document.get();

    traps.get = [scopeId, docPtr, el](const std::string& key, Value& out) -> bool {
        if (key == "document") {
            out = hostDocumentValue(docPtr);
            return true;
        }
        if (key == "frameElement") {
            out = hostElementValue(el);
            return true;
        }
        if (key == "window" || key == "self") {
            auto it = s_contentWindowProxies.find(scopeId);
            if (it != s_contentWindowProxies.end()) {
                out = it->second.get();
                return true;
            }
        }
        if (key == "parent" || key == "top") {
            ev::GlobalValue g = ev::globalValue("window");
            out = g.found ? g.value : ev::undefined();
            return true;
        }
        // Rooted across exitRealmScope: switching the scope back moves every
        // expando on and off globalThis, a chain of allocating calls that
        // leaves a raw Value read inside the frame's scope stale.
        enterRealmScope(scopeId);
        ev::GlobalValue g = ev::globalValue("globalThis");
        ev::Persistent value;
        if (g.found && ev::isObject(g.value)) {
            value.set(ev::getProperty(g.value, key));
        }
        exitRealmScope();
        if (ev::isUndefined(value.get())) return false;
        // A function of the frame's realm runs there, whoever calls it.
        out = ev::isFunction(value.get()) ? wrapRealmFunction(scopeId, docPtr, value.get()) : value.get();
        return true;
    };

    traps.set = [scopeId](const std::string& key, Value v) {
        // Rooted: globalValue may allocate (embed.h) before the write.
        ev::Persistent value(v);
        enterRealmScope(scopeId);
        ev::GlobalValue g = ev::globalValue("globalThis");
        if (g.found && ev::isObject(g.value)) {
            ev::setProperty(g.value, key, value.get());
        }
        exitRealmScope();
    };

    traps.has = [scopeId](const std::string& key) -> bool {
        if (key == "document" || key == "frameElement" || key == "window" || key == "self" || key == "parent" || key == "top") {
            return true;
        }
        enterRealmScope(scopeId);
        ev::GlobalValue g = ev::globalValue("globalThis");
        bool hasProp = false;
        if (g.found && ev::isObject(g.value)) {
            Value v = ev::getProperty(g.value, key);
            hasProp = !ev::isUndefined(v);
        }
        exitRealmScope();
        return hasProp;
    };

    traps.ownKeys = [scopeId]() -> std::vector<std::string> {
        std::vector<std::string> keys = {"document", "frameElement", "window", "self", "parent", "top"};
        enterRealmScope(scopeId);
        // Each value rooted before the next lookup: globalValue, getProperty,
        // call and getElement may all allocate.
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::Persistent global(gt.value);
            ev::GlobalValue objG = ev::globalValue("Object");
            if (objG.found && ev::isObject(objG.value)) {
                ev::Persistent objectNs(objG.value);
                ev::Persistent namesFn(ev::getProperty(objectNs.get(), "getOwnPropertyNames"));
                if (ev::isFunction(namesFn.get())) {
                    const Value arg = global.get();
                    ev::CallResult res = ev::call(namesFn.get(), objectNs.get(), std::span<const Value>(&arg, 1));
                    if (!res.thrown && ev::isObject(res.value)) {
                        ev::Persistent names(res.value);
                        Value lenVal = ev::getProperty(names.get(), "length");
                        int len = ev::isNumber(lenVal) ? satCast<int>(ev::toDouble(lenVal)) : 0;
                        for (int i = 0; i < len; ++i) {
                            Value k = ev::getElement(names.get(), i);
                            if (ev::isString(k)) keys.push_back(ev::toUtf8(k));
                        }
                    }
                }
            }
        }
        exitRealmScope();
        return keys;
    };

    Value proxy = makeHostProxy(std::move(traps));
    s_contentWindowProxies[scopeId].set(proxy);
    return proxy;
}

} // namespace

void clearContentWindowProxy(uint64_t scopeId) {
    if (scopeId == 0) {
        s_contentWindowProxies.clear();
        s_realmFunctions.clear();
    } else {
        s_contentWindowProxies.erase(scopeId);
        s_realmFunctions.erase(scopeId);
    }
}

void decorateIFrameProto(ObjectBuilder& b) {
    b.accessor("src",
        [](Value self, std::span<const Value>) -> Value {
            dom::Element* el = hostElementOf(self);
            return el ? ev::fromUtf8(el->getAttribute("src")) : ev::fromUtf8("");
        },
        [](Value self, std::span<const Value> a) -> Value {
            dom::Element* el = hostElementOf(self);
            if (!el) return ev::undefined();
            std::string src = ev::toUtf8(argAt(a, 0));
            el->setAttribute("src", src);
            if (engine::Engine* engine = hostEngine()) {
                if (engine::IframeDoc* d = engine->iframeDocForElement(el)) {
                    if (d->document) {
                        clearContentWindowProxy(scopeIdForDocument(d->document.get()));
                    }
                }
                engine->reloadIframe(el);
            }
            return ev::undefined();
        });

    b.accessor("width",
        [](Value self, std::span<const Value>) -> Value {
            dom::Element* el = hostElementOf(self);
            return el ? ev::fromUtf8(el->getAttribute("width")) : ev::fromUtf8("");
        },
        [](Value self, std::span<const Value> a) -> Value {
            dom::Element* el = hostElementOf(self);
            if (el) el->setAttribute("width", ev::toUtf8(argAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("height",
        [](Value self, std::span<const Value>) -> Value {
            dom::Element* el = hostElementOf(self);
            return el ? ev::fromUtf8(el->getAttribute("height")) : ev::fromUtf8("");
        },
        [](Value self, std::span<const Value> a) -> Value {
            dom::Element* el = hostElementOf(self);
            if (el) el->setAttribute("height", ev::toUtf8(argAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("contentWindow",
        [](Value self, std::span<const Value>) -> Value {
            dom::Element* el = hostElementOf(self);
            auto* eng = hostEngine();
            if (!el || !eng) return ev::null();
            engine::IframeDoc* d = eng->iframeDocForElement(el);
            if (!d || !d->document) return ev::null();
            return makeContentWindowProxy(el, d);
        },
        nullptr);

    // The sub-document's own `document` value (the one its scripts see), so
    // the host can reach into a same-app frame the way the web allows for a
    // same-origin one; null until the frame has loaded.
    b.accessor("contentDocument",
        [](Value self, std::span<const Value>) -> Value {
            dom::Element* el = hostElementOf(self);
            auto* eng = hostEngine();
            if (!el || !eng) return ev::null();
            engine::IframeDoc* d = eng->iframeDocForElement(el);
            if (!d || !d->document) return ev::null();
            return hostDocumentValue(d->document.get());
        },
        nullptr);

    b.def("reload", 0, [](Value self, std::span<const Value>) -> Value {
        dom::Element* el = hostElementOf(self);
        if (el && (el->tagName() == "IFRAME" || el->tagName() == "iframe")) {
            if (engine::Engine* engine = hostEngine()) {
                if (engine::IframeDoc* d = engine->iframeDocForElement(el)) {
                    if (d->document) {
                        clearContentWindowProxy(scopeIdForDocument(d->document.get()));
                    }
                }
                engine->reloadIframe(el);
            }
        }
        return ev::undefined();
    });

    b.def("capture", 0, [](Value self, std::span<const Value>) -> Value {
        dom::Element* el = hostElementOf(self);
        if (!el || (el->tagName() != "IFRAME" && el->tagName() != "iframe")) return ev::null();
        engine::Engine* engine = hostEngine();
        if (!engine) return ev::null();
        int w = 0, h = 0;
        auto pixels = engine->captureIframe(el, w, h);
        if (pixels.empty() || w <= 0 || h <= 0) return ev::null();
        return makeImageDataValue(w, h, pixels.data());
    });
}

void runHostSubDocScripts(engine::Engine& engine, dom::Document* subDoc,
                          const std::vector<engine::ScriptEntry>& scripts,
                          const std::string& appDir, const std::string& basePath,
                          bool isChild) {
    if (scripts.empty() || !subDoc) return;

    uint64_t scopeId = scopeIdForDocument(subDoc);
    enterRealmScope(scopeId);
    dom::Document* prevHostDoc = currentHostDocument();
    setCurrentHostDocument(subDoc);
    reloadHostStorage(basePath);

    // Rooted: running the scripts allocates, and the restore must name the
    // previous document's CURRENT address; globalThis is re-read each time.
    ev::GlobalValue docG = ev::globalValue("document");
    ev::Persistent prevDoc(docG.found ? docG.value : ev::null());
    setDocumentGlobal(hostDocumentValue(subDoc));

    // Module and classic scripts alike, in document order, as the main
    // document runs its scripts (Engine::initAppRealm). Every script is
    // compiled as a bronze module, so a module script needs nothing extra:
    // its imports resolve against its own file, or against the frame's
    // document for an inline one (the "<inline>" name sits in basePath), and
    // the realm's module registry evaluates a module that two scripts import
    // once. Each script is its own unit, so a module's top-level bindings stay
    // in its own scope.
    (void)appDir;
    for (const auto& s : scripts) {
        std::string code = s.isInline() ? s.code : engine::AppLoader::loadFile(s.path);
        if (code.empty()) continue;
        std::string fname = s.isInline() ? (basePath + "/<inline>") : s.path;
        evalScript(engine, code, fname);
    }

    if (!ev::isNull(prevDoc.get())) setDocumentGlobal(prevDoc.get());

    setCurrentHostDocument(prevHostDoc);
    exitRealmScope();
}

namespace {
// globalThis.<hook>() in the document's realm. The global is rooted across
// the property read (a getter may allocate) so the call's receiver is current.
void callRealmHook(dom::Document* doc, const char* hook) {
    if (!doc) return;
    dom::Document* prevHostDoc = currentHostDocument();
    setCurrentHostDocument(doc);
    uint64_t scopeId = scopeIdForDocument(doc);
    enterRealmScope(scopeId);
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::Persistent global(gt.value);
        ev::Persistent fn(ev::getProperty(global.get(), hook));
        if (ev::isFunction(fn.get())) {
            ev::call(fn.get(), global.get(), {});
        }
    }
    exitRealmScope();
    setCurrentHostDocument(prevHostDoc);
}
}  // namespace

void triggerPanelsReady(dom::Document* doc) { callRealmHook(doc, "__onPanelsReady"); }

void triggerSplashDismiss(dom::Document* doc) { callRealmHook(doc, "__onDismiss"); }

} // namespace bro::bronze_host
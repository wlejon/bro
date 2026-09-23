#include "bronze_host/host_iframe.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
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
        enterRealmScope(scopeId);
        ev::GlobalValue g = ev::globalValue("globalThis");
        bool found = false;
        if (g.found && ev::isObject(g.value)) {
            Value v = ev::getProperty(g.value, key);
            if (!ev::isUndefined(v)) {
                out = v;
                found = true;
            }
        }
        exitRealmScope();
        return found;
    };

    traps.set = [scopeId](const std::string& key, Value v) {
        enterRealmScope(scopeId);
        ev::GlobalValue g = ev::globalValue("globalThis");
        if (g.found && ev::isObject(g.value)) {
            ev::setProperty(g.value, key, v);
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
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::GlobalValue objG = ev::globalValue("Object");
            if (objG.found && ev::isObject(objG.value)) {
                Value getOwnPropertyNamesFn = ev::getProperty(objG.value, "getOwnPropertyNames");
                if (ev::isFunction(getOwnPropertyNamesFn)) {
                    Value arg = gt.value;
                    ev::CallResult res = ev::call(getOwnPropertyNamesFn, objG.value, std::span<const Value>(&arg, 1));
                    if (!res.thrown && ev::isObject(res.value)) {
                        Value lenVal = ev::getProperty(res.value, "length");
                        int len = static_cast<int>(ev::toDouble(lenVal));
                        for (int i = 0; i < len; ++i) {
                            Value k = ev::getElement(res.value, i);
                            keys.push_back(ev::toUtf8(k));
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
    } else {
        s_contentWindowProxies.erase(scopeId);
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
    auto setDocumentGlobal = [](Value docIn) {
        ev::Persistent doc(docIn);
        ev::registerGlobal("document", doc.get());
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::setProperty(gt.value, "document", doc.get());
        }
    };
    setDocumentGlobal(hostDocumentValue(subDoc));

    for (const auto& s : scripts) {
        if (s.isModule) {
            LOG_WARN("subdoc '%s': <script type=module> not yet supported, skipping",
                     appDir.c_str());
            continue;
        }
        std::string code = s.isInline() ? s.code : engine::AppLoader::loadFile(s.path);
        if (code.empty()) continue;
        std::string fname = s.isInline() ? (basePath + "/<inline>") : s.path;
        evalScript(engine, code, fname);
    }

    if (!ev::isNull(prevDoc.get())) setDocumentGlobal(prevDoc.get());

    setCurrentHostDocument(prevHostDoc);
    exitRealmScope();
}

void triggerPanelsReady(dom::Document* doc) {
    if (!doc) return;
    uint64_t scopeId = scopeIdForDocument(doc);
    enterRealmScope(scopeId);
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        Value fn = ev::getProperty(gt.value, "__onPanelsReady");
        if (ev::isFunction(fn)) {
            ev::call(fn, gt.value, {});
        }
    }
    exitRealmScope();
}

void triggerSplashDismiss(dom::Document* doc) {
    if (!doc) return;
    uint64_t scopeId = scopeIdForDocument(doc);
    enterRealmScope(scopeId);
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        Value fn = ev::getProperty(gt.value, "__onDismiss");
        if (ev::isFunction(fn)) {
            ev::call(fn, gt.value, {});
        }
    }
    exitRealmScope();
}

} // namespace bro::bronze_host
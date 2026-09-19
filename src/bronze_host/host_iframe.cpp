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
        [](Value, std::span<const Value>) -> Value {
            return ev::null();
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

    ev::GlobalValue docG = ev::globalValue("document");
    ev::GlobalValue gt = ev::globalValue("globalThis");
    Value prevDoc = docG.found ? docG.value : ev::null();

    Value subDocVal = hostDocumentValue(subDoc);
    ev::registerGlobal("document", subDocVal);
    if (gt.found && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "document", subDocVal);
    }

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

    if (!ev::isNull(prevDoc)) {
        ev::registerGlobal("document", prevDoc);
        if (gt.found && ev::isObject(gt.value)) {
            ev::setProperty(gt.value, "document", prevDoc);
        }
    }

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
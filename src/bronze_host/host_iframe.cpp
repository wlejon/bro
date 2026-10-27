// <iframe src=dir>: a sub-document running as a compartment of its own
// (host_compartment.h). It is the app's own, so the host page reaches its
// window and document as the web's same-origin frames are reached; an
// <appview> is another app, reached only by message: its contentWindow is a
// window proxy carrying postMessage (host_membrane.h), and its contentDocument
// is null, as a cross-origin frame's is.

#include "bronze_host/host_iframe.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_document.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_membrane.h"
#include "bronze_host/host_appview.h"
#include "bronze_host/host_storage.h"
#include "dom/element.h"
#include "engine/app_loader.h"
#include "engine/engine.h"
#include "engine/window_host.h"
#include "util/log.h"

namespace bro::bronze_host {

namespace {

// The compartment an <iframe> / <appview> element's sub-document runs in, or
// null before it has loaded (or once it is stopped).
Compartment* frameCompartment(dom::Element* el) {
    engine::Engine* eng = hostEngine();
    if (!el || !eng) return nullptr;
    engine::IframeDoc* d = eng->iframeDocForElement(el);
    if (!d || !d->document) return nullptr;
    Compartment* c = compartmentForDocument(d->document.get());
    return c && c->document == d->document.get() && !c->dying ? c : nullptr;
}

}  // namespace

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
            // The old document's compartment goes with it at the drain
            // (teardownSubDoc); the new one is made when its scripts run.
            if (engine::Engine* engine = hostEngine()) engine->reloadIframe(el);
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

    // The frame's window: itself to the app's own page, a proxy carrying
    // postMessage (and `closed`) to another app's (windowProxyFor). Null
    // until it has loaded.
    b.accessor("contentWindow",
        [](Value self, std::span<const Value>) -> Value {
            Compartment* c = frameCompartment(hostElementOf(self));
            return c ? windowProxyFor(c) : ev::null();
        },
        nullptr);

    // Its document, to the app's own page; null to another app's, as a
    // cross-origin frame's is.
    b.accessor("contentDocument",
        [](Value self, std::span<const Value>) -> Value {
            Compartment* c = frameCompartment(hostElementOf(self));
            if (!c || !c->realm || !sameApp(currentCompartment(), c)) return ev::null();
            ev::Persistent global(ev::getRealmGlobal(c->realm));
            return ev::getProperty(global.get(), "document");
        },
        nullptr);

    b.def("reload", 0, [](Value self, std::span<const Value>) -> Value {
        dom::Element* el = hostElementOf(self);
        if (el && el->isFrameHost()) {
            if (engine::Engine* engine = hostEngine()) engine->reloadIframe(el);
        }
        return ev::undefined();
    });

    b.def("capture", 0, [](Value self, std::span<const Value>) -> Value {
        dom::Element* el = hostElementOf(self);
        if (!el || !el->isFrameHost()) return ev::null();
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
                          bool /*isChild*/) {
    if (!subDoc) return;

    // Every sub-document is a compartment of its own, made here as its
    // scripts are about to run (a scriptless one too: its window still
    // receives messages, and its events dispatch in its own realm).
    Compartment* c = compartmentForDocument(subDoc);
    if (c->document != subDoc || c->kind == CompartmentKind::App) {
        CompartmentSetup setup;
        setup.document = subDoc;
        setup.appDir = appDir;
        setup.basePath = basePath;
        if (engine::IframeDoc* f = engine.iframeForDocument(subDoc)) {
            // Frames and appviews are hosted by the app's own document only
            // (Engine::syncIframes).
            setup.parent = &appCompartment();
            setup.hostElement = f->element;
            if (f->element && (f->element->tagName() == "appview" || f->element->tagName() == "APPVIEW")) {
                setup.kind = CompartmentKind::AppView;
                prepareAppView(engine, setup);
            } else {
                setup.kind = CompartmentKind::Frame;
            }
        } else if (engine::WindowHost* wh = engine.windowHostForDocument(subDoc)) {
            setup.kind = CompartmentKind::Window;
            setup.windowHostId = wh->id;
        } else {
            setup.kind = CompartmentKind::Panel;
        }
        c = createCompartment(engine, std::move(setup));
    }

    // Module and classic scripts alike, in document order, as the main
    // document runs its scripts (Engine::initAppRealm). Every script is
    // compiled as a bronze module, so a module script needs nothing extra:
    // its imports resolve against its own file, or against the frame's
    // document for an inline one (the "<inline>" name sits in basePath), and
    // the realm's module registry evaluates a module that two scripts import
    // once. Each script is its own unit, so a module's top-level bindings stay
    // in its own scope. One entry of the compartment's: an appview's may yield
    // at its slice and finish on later frames.
    runEntry(c, EntryKind::Async, [&engine, scripts, basePath] {
        reloadHostStorage(basePath);
        for (const auto& s : scripts) {
            std::string code = s.isInline() ? s.code : engine::AppLoader::loadFile(s.path);
            if (code.empty()) continue;
            std::string fname = s.isInline() ? (basePath + "/<inline>") : s.path;
            evalScript(engine, code, fname);
        }
    });
}

namespace {
// globalThis.<hook>() in the document's compartment. The global is rooted
// across the property read (a getter may allocate) so the call's receiver is
// current.
void callRealmHook(dom::Document* doc, const char* hook) {
    if (!doc) return;
    runEntry(doc, EntryKind::Sync, [hook] {
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (!gt.found || !ev::isObject(gt.value)) return;
        ev::Persistent global(gt.value);
        ev::Persistent fn(ev::getProperty(global.get(), hook));
        if (ev::isFunction(fn.get())) {
            ev::CallResult r = ev::call(fn.get(), global.get(), {});
            if (r.thrown) reportBronzeError(hook, r.value);
        }
    });
}
}  // namespace

void triggerPanelsReady(dom::Document* doc) { callRealmHook(doc, "__onPanelsReady"); }

void triggerSplashDismiss(dom::Document* doc) { callRealmHook(doc, "__onDismiss"); }

} // namespace bro::bronze_host

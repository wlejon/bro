#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "platform/sdl_window.h"

#include <cctype>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

dom::Document* documentFor(dom::Document* fixed) {
    if (fixed) return fixed;
    auto* e = hostEngine();
    return e ? e->document() : nullptr;
}

Value wrapElement(dom::Element* el) {
    return hostElementValue(el);
}

Value createElementImpl(dom::Document* fixed, std::span<const Value> a,
                        size_t tagIndex) {
    Value tagV = argAt(a, tagIndex);
    if (ev::isObject(tagV)) return ev::throwTypeError("createElement: tag must be a string");
    std::string tag = ev::toUtf8(tagV);
    for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    dom::Document* doc = documentFor(fixed);
    if (!doc) return ev::throwError("bronze host: engine has no document");
    dom::Element* el = doc->createElement(tag);
    if (!el) return ev::throwError("bronze host: createElement failed");
    return hostElementValue(el);
}

}  // namespace

Value makeDocumentValue(dom::Document* fixed) {
    ObjectBuilder b;
    b.def("createElement", 1, [fixed](Value, std::span<const Value> a) {
        return createElementImpl(fixed, a, 0);
    });
    b.def("createElementNS", 2, [fixed](Value, std::span<const Value> a) {
        return createElementImpl(fixed, a, 1);
    });
    b.def("createTextNode", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        Value v = argAt(a, 0);
        std::string text =
            (ev::isObject(v) || ev::isUndefined(v)) ? "" : ev::toUtf8(v);
        return hostNodeValue(doc->createTextNode(text));
    });
    b.def("createComment", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        Value v = argAt(a, 0);
        std::string text =
            (ev::isObject(v) || ev::isUndefined(v)) ? "" : ev::toUtf8(v);
        return hostNodeValue(doc->createComment(text));
    });
    b.def("createDocumentFragment", 0, [fixed](Value, std::span<const Value>) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        return hostNodeValue(doc->createDocumentFragment());
    });
    b.def("getElementById", 1, [fixed](Value, std::span<const Value> a) {
        Value idV = argAt(a, 0);
        if (ev::isObject(idV) || ev::isUndefined(idV)) return ev::null();
        std::string id = ev::toUtf8(idV);
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        dom::Element* el = doc->getElementById(id);
        return wrapElement(el);
    });
    b.def("querySelector", 1, [fixed](Value, std::span<const Value> a) {
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return ev::null();
        std::string sel = ev::toUtf8(selV);
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        dom::Element* el = doc->querySelector(sel);
        return wrapElement(el);
    });
    b.def("querySelectorAll", 1, [fixed](Value, std::span<const Value> a) {
        auto empty = []() {
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        };
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return empty();
        dom::Document* doc = documentFor(fixed);
        if (!doc) return empty();
        std::vector<dom::Element*> list = doc->querySelectorAll(ev::toUtf8(selV));
        return hostArrayOf(list.size(),
                           [&list](size_t i) { return hostElementValue(list[i]); });
    });

    auto defDocElement = [&b, fixed](const char* name,
                                     dom::Element* (dom::Document::*get)() const) {
        b.accessor(name,
                   [get, fixed](Value, std::span<const Value>) {
                       dom::Document* doc = documentFor(fixed);
                       if (!doc) return ev::null();
                       return hostElementValue((doc->*get)());
                   },
                   nullptr);
    };
    defDocElement("body", &dom::Document::body);
    defDocElement("documentElement", &dom::Document::documentElement);
    b.accessor("activeElement",
               [fixed](Value, std::span<const Value>) {
                   dom::Document* doc = documentFor(fixed);
                   if (!doc) return ev::null();
                   return hostElementValue(doc->activeElement());
               },
               nullptr);
    b.accessor("pointerLockElement",
               [fixed](Value, std::span<const Value>) {
                   if (fixed) return ev::null();
                   auto* e = hostEngine();
                   if (!e) return ev::null();
                   return hostElementValue(e->pointerLockElement());
               },
               nullptr);
    b.def("exitPointerLock", 0, [fixed](Value, std::span<const Value>) {
        if (!fixed) {
            if (auto* e = hostEngine()) {
                e->exitPointerLock();
            }
        }
        return ev::undefined();
    });
    b.accessor("fullscreenElement",
               [fixed](Value, std::span<const Value>) {
                   if (fixed) return ev::null();
                   return hostElementValue(hostFullscreenElement());
               },
               nullptr);
    b.accessor("fullscreenEnabled",
               [](Value, std::span<const Value>) {
                   return ev::fromBool(true);
               },
               nullptr);
    b.def("exitFullscreen", 0, [fixed](Value, std::span<const Value>) {
        if (!fixed) {
            setHostFullscreenElement(nullptr);
            if (auto* e = hostEngine()) {
                e->setFullscreenState(false);
                if (auto* win = e->window()) {
                    win->setFullscreen(false);
                }
            }
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    installElementEventTarget(b, [fixed]() -> dom::Element* {
        dom::Document* doc = documentFor(fixed);
        return doc ? doc->documentElement() : nullptr;
    }, "document");
    return b.get();
}

Value hostDocumentValue(dom::Document* doc) {
    return makeDocumentValue(doc);
}

}  // namespace bro::bronze_host

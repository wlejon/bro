// `__bro_cssom` — the host half of the CSSOM. The objects (document.styleSheets,
// CSSStyleSheet, the CSSRule family, constructable sheets and
// adoptedStyleSheets) are JavaScript (js/cssom.js); the sheets they describe
// are the document's (dom/document_cssom.cpp), and this is the door:
//
//   hasSheet(el)              whether a <style> / <link> element has a sheet
//   sheetText(el)             its CSS text (a CSSOM edit's, once made)
//   setSheetText(el, css)     its rules are now `css`; the cascade is rebuilt
//   sheetDisabled(el)         its disabled flag
//   setSheetDisabled(el, b)
//   setAdopted(scope, texts)  the constructed sheets adopted by the document
//                             (scope null) or by a shadow root, in order

#include "bronze_host/host_builder.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_node.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/shadow_root.h"

#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// The element and the document whose sheet it is, or nulls.
dom::Document* docOf(dom::Element* el) {
    if (!el) return nullptr;
    dom::Document* d = el->document();
    if (!d) d = currentHostDocument();
    if (d) d->syncSheets();
    return d;
}

}  // namespace

void installCssomHooks() {
    ObjectBuilder b;
    b.def("hasSheet", 1, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        dom::Document* d = docOf(el);
        return ev::fromBool(d && d->hasOwnedSheet(el));
    });
    b.def("sheetText", 1, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        dom::Document* d = docOf(el);
        return ev::fromUtf8(d ? d->ownedSheetText(el) : std::string());
    });
    b.def("setSheetText", 2, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        dom::Document* d = docOf(el);
        if (d) d->setOwnedSheetText(el, ev::toUtf8(argAt(a, 1)));
        return ev::undefined();
    });
    b.def("sheetDisabled", 1, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        dom::Document* d = docOf(el);
        return ev::fromBool(d && d->ownedSheetDisabled(el));
    });
    b.def("setSheetDisabled", 2, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        dom::Document* d = docOf(el);
        if (d) d->setOwnedSheetDisabled(el, boolAt(a, 1));
        return ev::undefined();
    });
    b.def("setAdopted", 2, [](Value, std::span<const Value> a) {
        void* scope = nullptr;
        Value scopeV = argAt(a, 0);
        if (ev::isObject(scopeV)) {
            auto* sr = dynamic_cast<dom::ShadowRoot*>(hostNodeOf(scopeV));
            if (!sr) return ev::undefined();
            scope = sr;
        }
        std::vector<std::string> texts;
        Value list = argAt(a, 1);
        if (ev::isObject(list)) {
            const ev::Persistent listRoot(list);
            const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(listRoot.get(), "length")));
            for (uint32_t i = 0; i < n; ++i) texts.push_back(ev::toUtf8(ev::getElement(listRoot.get(), i)));
        }
        if (dom::Document* d = currentHostDocument()) d->setAdoptedSheets(scope, texts);
        return ev::undefined();
    });
    ev::registerGlobal("__bro_cssom", b.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "__bro_cssom", b.get());
}

}  // namespace bro::bronze_host

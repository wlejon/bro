#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_list_iteration.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_dom_lists.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_document.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_range.h"
#include "bronze_host/host_selection.h"
#include "bronze_host/host_matchmedia.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_web_animations.h"
#include "bronze_host/host_web_globals.h"

#include "engine/app_loader.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "platform/window.h"

#include "canvas/canvas_scene.h"
#include "layout/form_control.h"
#include "util/log.h"

#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "dom/node.h"
#include "dom/range.h"

namespace bro::bronze_host {

void fireElementCloned(dom::Document* doc, dom::Element* src, dom::Element* clone) {
    if (!doc || !src || !clone) return;

    if (src->tagName() == "SELECT" || src->tagName() == "select") {
        int want = layout::selectedIndex(src);
        if (want >= 0) {
            int idx = 0;
            for (auto* opt : clone->children()) {
                const std::string& t = opt->tagName();
                if (t != "OPTION" && t != "option") continue;
                if (idx == want) opt->setAttribute("selected", "");
                else if (opt->hasAttribute("selected")) opt->removeAttribute("selected");
                ++idx;
            }
            clone->setPendingSelectedIndex(want);
        }
    }

    auto* srcScene = static_cast<canvas::CanvasScene*>(src->canvasScene());
    if (srcScene) {
        int w = srcScene->width(), h = srcScene->height();
        if (w > 0 && h > 0) {
            auto pixels = srcScene->getImageData(0, 0, w, h);
            if (!pixels.empty()) {
                if (auto* eng = hostEngine()) {
                    auto* dstScene = eng->createCanvasContext(clone);
                    if (dstScene) {
                        dstScene->putImageData(pixels.data(), w, h, 0, 0);
                    }
                }
            }
        }
    }
}

namespace {

// The document a `document` object acts on: the one it was made for, or, for
// the app's (made with none), whatever the engine is showing (a reload swaps
// it). Every compartment's `document` is its own object over its own
// document, so nothing here follows the running code.
dom::Document* documentFor(dom::Document* fixed) {
    if (fixed) {
        if (!dom::Document::isLiveDocument(fixed)) return nullptr;
        hostSweepTouchDocument(fixed);
    }
    dom::Document* doc = fixed ? fixed : (hostEngine() ? hostEngine()->document() : nullptr);
    if (doc) doc->setElementClonedCallback(&fireElementCloned);
    return doc;
}

}  // namespace

// The running code's document: its compartment's (host_compartment.h).
dom::Document* currentHostDocument() {
    Compartment* c = currentCompartment();
    if (c && c->document) return c->document;
    return hostEngine() ? hostEngine()->document() : nullptr;
}

namespace {

Value wrapElement(dom::Element* el) {
    return hostElementValue(el);
}

// The element a pointer at client (x, y) would target: the same hit test the
// input pipeline runs, so elementFromPoint and a click there always agree
// (stacking order, clips, pointer-events, fixed boxes against the viewport).
// Where no box is hit, the document element answers, as on the web.
dom::Element* hitTestClientPoint(engine::Engine* e, dom::Document* doc, float x, float y) {
    if (!e) return nullptr;
    float vw = static_cast<float>(e->contentWidth());
    float vh = static_cast<float>(e->contentHeight());
    if (x < 0.0f || y < 0.0f || x >= vw || y >= vh) return nullptr;
    if (doc) {
        e->flushLayoutForRead(doc);
    }
    dom::Element* hit = e->hitTest(x, y + e->viewportScrollY());
    if (!hit && doc) {
        hit = doc->documentElement();
    }
    return hit;
}

Value createElementImpl(dom::Document* fixed, std::span<const Value> a,
                        size_t tagIndex) {
    Value tagV = argAt(a, tagIndex);
    if (ev::isObject(tagV)) return ev::throwTypeError("createElement: tag must be a string");
    std::string tag = ev::toUtf8(tagV);
    // createElementNS(ns, qualifiedName): the namespace is argument 0 (null /
    // undefined / "" are the null namespace) and a `prefix:` is dropped from
    // the local name the element is made with.
    //
    // Only createElement lower-cases (it makes HTML elements in an HTML
    // document). createElementNS keeps the qualified name EXACTLY as given —
    // `linearGradient` stays camelCase in tagName and localName, as the SVG
    // paint path and every SVG library expect — while the engine's internal
    // key (dom::Element::tagName) is still the case-folded local name.
    const bool withNs = tagIndex == 1;
    std::string nsUri;
    const std::string qualifiedName = tag;
    if (withNs) {
        Value nsV = argAt(a, 0);
        if (!ev::isNull(nsV) && !ev::isUndefined(nsV)) nsUri = ev::toUtf8(nsV);
        size_t colon = tag.find(':');
        if (colon != std::string::npos) {
            // DOM "validate and extract": a prefix needs a namespace, and
            // xml / xmlns prefixes need theirs.
            const std::string prefix = tag.substr(0, colon);
            if (nsUri.empty() ||
                (prefix == "xml" && nsUri != "http://www.w3.org/XML/1998/namespace") ||
                (prefix == "xmlns" && nsUri != "http://www.w3.org/2000/xmlns/")) {
                return ev::throwValue(hostMakeDomError(
                    "NamespaceError", "createElementNS: '" + qualifiedName +
                                          "' has a prefix its namespace does not allow"));
            }
            tag = tag.substr(colon + 1);
        }
        if (tag.empty()) {
            return ev::throwValue(hostMakeDomError(
                "InvalidCharacterError", "createElementNS: '" + qualifiedName +
                                             "' is not a valid qualified name"));
        }
    }
    for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    dom::Document* doc = documentFor(fixed);
    if (!doc) return ev::throwError("bronze host: engine has no document");
    dom::Element* el = doc->createElement(tag);
    if (!el) return ev::throwError("bronze host: createElement failed");
    if (withNs) {
        el->setNamespaceURI(nsUri);
        el->setQualifiedName(qualifiedName);
    }
    Value customVal = constructCustomElement(el, tag);
    if (!ev::isUndefined(customVal)) {
        return customVal;
    }
    return hostElementValue(el);
}

bool elementMatchesSelector(dom::Element* el, const std::string& selector) {
    if (!el) return false;
    if (selector.empty()) return false;
    if (selector == "*") return true;
    if (selector.rfind("name:", 0) == 0) {
        std::string name = selector.substr(5);
        return el->getAttribute("name") == name;
    }
    if (selector[0] == '.') {
        std::string cls = selector.substr(1);
        std::istringstream iss(cls);
        std::string tok;
        const std::string& s = el->className();
        while (iss >> tok) {
            size_t pos = 0;
            bool found = false;
            while ((pos = s.find(tok, pos)) != std::string::npos) {
                bool startOk = (pos == 0 || std::isspace(static_cast<unsigned char>(s[pos - 1])));
                bool endOk = (pos + tok.size() == s.size() || std::isspace(static_cast<unsigned char>(s[pos + tok.size()])));
                if (startOk && endOk) { found = true; break; }
                pos += tok.size();
            }
            if (!found) return false;
        }
        return true;
    }
    // getElementsByTagName's qualified name (DOM §4.2.6.1): an HTML element
    // matches it lower-cased, any other (SVG, an XML document's) exactly.
    const std::string qname = el->qualifiedName();
    if (el->ns() != dom::Element::Namespace::HTML) return qname == selector;
    if (qname.size() != selector.size()) return false;
    for (size_t i = 0; i < qname.size(); ++i) {
        if (qname[i] != std::tolower(static_cast<unsigned char>(selector[i]))) return false;
    }
    return true;
}

void collectMatching(dom::Element* cur, const std::string& selector, std::vector<dom::Element*>& out) {
    if (!cur) return;
    if (elementMatchesSelector(cur, selector)) {
        out.push_back(cur);
    }
    for (dom::Element* child : cur->children()) {
        collectMatching(child, selector, out);
    }
}

bool isHtmlTag(const dom::Element* el, const char* tag) {
    return el->ns() == dom::Element::Namespace::HTML && el->qualifiedName() == tag;
}

void collectWhere(dom::Element* cur, bool (*keep)(const dom::Element*), std::vector<dom::Element*>& out) {
    if (keep(cur)) out.push_back(cur);
    for (dom::Element* child : cur->children()) collectWhere(child, keep, out);
}

std::vector<dom::Element*> runCollectionQuery(dom::Element* root, dom::Document* fixed, const std::string& selector) {
    std::vector<dom::Element*> results;
    if (root) {
        for (dom::Element* child : root->children()) {
            collectMatching(child, selector, results);
        }
    } else {
        dom::Document* doc = documentFor(fixed);
        if (doc && doc->documentElement()) {
            collectMatching(doc->documentElement(), selector, results);
        }
    }
    return results;
}

}  // namespace

Value makeLiveHTMLCollection(dom::Element* root, dom::Document* fixed, std::string selector) {
    return makeLiveCollection([root, fixed, selector]() { return runCollectionQuery(root, fixed, selector); });
}

namespace {

// The legacy event interfaces document.createEvent accepts, lowercased; every
// one of them yields an uninitialized Event the caller initEvent()s, except
// "customevent" which yields a CustomEvent so `detail` is there to set.
bool isCreateEventInterface(const std::string& lower) {
    static const char* const kNames[] = {
        "event", "events", "htmlevents", "svgevents", "customevent", "uievent", "uievents",
        "mouseevent", "mouseevents", "keyboardevent", "keyevents", "touchevent", "focusevent",
        "inputevent", "wheelevent", "pointerevent", "dragevent", "compositionevent", "messageevent",
    };
    for (const char* n : kNames) if (lower == n) return true;
    return false;
}

// The page's classic <script>s, per document, as its classic scripts
// started: a script element, HTML, whose type is classic JavaScript, with a
// src (not a .lua file) or a body — the AppLoader manifest's classic scripts,
// one for one, in document order.
std::unordered_map<dom::Document*, std::vector<dom::Element*>>& classicScriptSnapshots() {
    static std::unordered_map<dom::Document*, std::vector<dom::Element*>> snapshots;
    return snapshots;
}

std::vector<dom::Element*> classicScriptElements(dom::Document* doc) {
    std::vector<dom::Element*> out;
    for (dom::Element* el : doc->querySelectorAll("script")) {
        if (el->ns() != dom::Element::Namespace::HTML) continue;
        std::string type = el->getAttribute("type");
        for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t b = type.find_first_not_of(" \t\r\n");
        type = b == std::string::npos ? std::string() : type.substr(b, type.find_last_not_of(" \t\r\n") - b + 1);
        if (!engine::AppLoader::isClassicScriptType(type)) continue;
        if (el->hasAttribute("src")) {
            const std::string src = el->getAttribute("src");
            if (src.size() >= 4 && src.compare(src.size() - 4, 4, ".lua") == 0) continue;
        } else if (el->textContent().empty()) {
            continue;
        }
        out.push_back(el);
    }
    return out;
}

// document.currentScript: the classic script running now, else null. The
// snapshot's element is handed out only while it is still a script of the
// document, so a removed (and maybe freed) one is never touched.
Value currentScriptValue(dom::Document* doc) {
    const int k = currentClassicScript();
    if (!doc || k < 0) return ev::null();
    auto it = classicScriptSnapshots().find(doc);
    if (it == classicScriptSnapshots().end() || static_cast<size_t>(k) >= it->second.size()) return ev::null();
    dom::Element* el = it->second[static_cast<size_t>(k)];
    for (dom::Element* now : doc->querySelectorAll("script"))
        if (now == el) return hostElementValue(el);
    return ev::null();
}

std::string locationHref() {
    ev::GlobalValue loc = ev::globalValue("location");
    if (loc.found && ev::isObject(loc.value)) {
        Value href = ev::getProperty(loc.value, "href");
        if (ev::isString(href)) return ev::toUtf8(href);
    }
    return "bro://app/";
}

// hidden / visibilityState / location / URL / documentURI / implementation /
// createEvent / compatMode / characterSet / contentType: the document
// identity and lifecycle surface. Visibility follows the engine's page
// visibility (Engine::pageVisible, driven by focus and minimize, headless
// always visible) for every document the engine hosts, sub-documents
// included, since they share the window.
void decorateDocumentExtras(ObjectBuilder& b, dom::Document* fixed) {
    b.accessor("hidden", [fixed](Value, std::span<const Value>) {
        auto* e = hostEngine();
        dom::Document* doc = documentFor(fixed);
        return ev::fromBool(e ? !e->documentPageVisible(doc) : false);
    }, nullptr);
    b.accessor("visibilityState", [fixed](Value, std::span<const Value>) {
        auto* e = hostEngine();
        dom::Document* doc = documentFor(fixed);
        return ev::fromUtf8(e && !e->documentPageVisible(doc) ? "hidden" : "visible");
    }, nullptr);
    b.accessor("location", [](Value, std::span<const Value>) {
        ev::GlobalValue loc = ev::globalValue("location");
        return loc.found ? loc.value : ev::null();
    }, nullptr);
    b.accessor("URL", [](Value, std::span<const Value>) { return ev::fromUtf8(locationHref()); }, nullptr);
    b.accessor("currentScript", [fixed](Value, std::span<const Value>) {
        return currentScriptValue(documentFor(fixed));
    }, nullptr);
    b.accessor("documentURI", [](Value, std::span<const Value>) { return ev::fromUtf8(locationHref()); }, nullptr);
    b.set("compatMode", ev::fromUtf8("CSS1Compat"));
    b.set("characterSet", ev::fromUtf8("UTF-8"));
    b.set("charset", ev::fromUtf8("UTF-8"));
    b.set("contentType", ev::fromUtf8("text/html"));
    {
        ObjectBuilder impl;
        impl.def("createHTMLDocument", 1, [](Value, std::span<const Value> a) -> Value {
            Value tV = argAt(a, 0);
            std::string title = (ev::isUndefined(tV) || ev::isObject(tV)) ? "" : ev::toUtf8(tV);
            std::string html = "<!DOCTYPE html><html><head>";
            if (!title.empty()) html += "<title>" + title + "</title>";
            html += "</head><body></body></html>";
            return hostDocumentValue(parseIntoNewDocument(html));
        });
        impl.def("hasFeature", 2, [](Value, std::span<const Value>) { return ev::fromBool(true); });
        impl.def("createDocumentType", 3, [](Value, std::span<const Value>) { return ev::null(); });
        b.set("implementation", impl.get());
    }
    b.def("createEvent", 1, [](Value, std::span<const Value> a) -> Value {
        Value nV = argAt(a, 0);
        std::string name = (ev::isUndefined(nV) || ev::isObject(nV)) ? "" : ev::toUtf8(nV);
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!isCreateEventInterface(lower)) {
            return ev::throwError("NotSupportedError: document.createEvent('" + name +
                                  "') — no such event interface");
        }
        ev::GlobalValue ctor = ev::globalValue(lower == "customevent" ? "CustomEvent" : "Event");
        if (!ctor.found || !ev::isFunction(ctor.value)) {
            return ev::throwError("document.createEvent: the Event constructor is not installed");
        }
        Value type = ev::fromUtf8("");
        ev::CallResult r = ev::construct(ctor.value, std::span<const Value>(&type, 1));
        return r.value;
    });
}

}  // namespace

void decorateDocumentProto(ObjectBuilder& b) {
    b.def("elementFromPoint", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::null();
        float x = static_cast<float>(ev::toDouble(a[0]));
        float y = static_cast<float>(ev::toDouble(a[1]));
        auto* e = hostEngine();
        if (!e) return ev::null();
        dom::Document* doc = e->document();
        dom::Element* hit = hitTestClientPoint(e, doc, x, y);
        return wrapElement(hit);
    });
    b.def("elementsFromPoint", 2, [](Value, std::span<const Value> a) -> Value {
        auto emptyArr = []() { return hostArrayOf(0, [](size_t) { return ev::undefined(); }); };
        if (a.size() < 2) return emptyArr();
        float x = static_cast<float>(ev::toDouble(a[0]));
        float y = static_cast<float>(ev::toDouble(a[1]));
        auto* e = hostEngine();
        if (!e) return emptyArr();
        dom::Document* doc = e->document();
        dom::Element* hit = hitTestClientPoint(e, doc, x, y);
        if (!hit) return emptyArr();
        std::vector<dom::Element*> chain;
        for (dom::Element* cur = hit; cur; cur = cur->parentElement()) {
            chain.push_back(cur);
        }
        return hostArrayOf(chain.size(), [&chain](size_t i) {
            return hostElementValue(chain[i]);
        });
    });
}

Value makeDocumentValue(dom::Document* fixed) {
    ObjectBuilder b;
    b.set("nodeType", ev::fromDouble(9));
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
    b.def("createRange", 0, [fixed](Value, std::span<const Value>) {
        dom::Document* doc = documentFor(fixed);
        auto* r = new bro::dom::Range();
        if (doc) r->setDocument(doc);
        return wrapOwnedRange(r);
    });
    b.def("getSelection", 0, [fixed](Value, std::span<const Value>) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        return wrapSelection(doc->selection());
    });
    b.def("execCommand", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (!e || a.empty()) return ev::fromBool(false);
        std::string name = ev::toUtf8(a[0]);
        bool showUI = a.size() > 1 && ev::toBool(a[1]);
        std::string val = (a.size() > 2 && !ev::isObject(a[2]) && !ev::isUndefined(a[2]) && !ev::isNull(a[2]))
                              ? ev::toUtf8(a[2]) : "";
        return ev::fromBool(e->execCommand(name, showUI, val));
    });
    b.def("queryCommandEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (!e || a.empty()) return ev::fromBool(false);
        return ev::fromBool(e->queryCommandEnabled(ev::toUtf8(a[0])));
    });
    b.def("queryCommandSupported", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (!e || a.empty()) return ev::fromBool(false);
        return ev::fromBool(e->queryCommandSupported(ev::toUtf8(a[0])));
    });
    b.def("queryCommandState", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (!e || a.empty()) return ev::fromBool(false);
        return ev::fromBool(e->queryCommandState(ev::toUtf8(a[0])));
    });
    b.def("queryCommandValue", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine();
        if (!e || a.empty()) return ev::fromUtf8("");
        return ev::fromUtf8(e->queryCommandValue(ev::toUtf8(a[0])));
    });
    // Node.contains on the document: true for any node connected to it (and
    // the document itself). UIs ask `document.contains(el)` to tell whether an
    // element is still on the page before acting on it.
    b.def("contains", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        dom::Node* n = doc ? hostNodeOf(argAt(a, 0)) : nullptr;
        if (!n) return ev::fromBool(false);
        dom::Node* top = n;
        while (top->parentNode()) top = top->parentNode();
        return ev::fromBool(top == doc->documentElement());
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
        if (!dom::Element::isValidSelector(sel)) {
            return ev::throwValue(hostMakeDomError("SyntaxError", "'" + sel + "' is not a valid selector"));
        }
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        dom::Element* el = doc->querySelector(sel);
        return wrapElement(el);
    });
    b.def("querySelectorAll", 1, [fixed](Value, std::span<const Value> a) {
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return makeStaticNodeList({});
        std::string sel = ev::toUtf8(selV);
        if (!dom::Element::isValidSelector(sel)) {
            return ev::throwValue(hostMakeDomError("SyntaxError", "'" + sel + "' is not a valid selector"));
        }
        dom::Document* doc = documentFor(fixed);
        if (!doc) return makeStaticNodeList({});
        return makeStaticNodeList(doc->querySelectorAll(sel));
    });
    b.def("getElementsByTagName", 1, [fixed](Value, std::span<const Value> a) {
        std::string tag = !a.empty() && !ev::isUndefined(a[0]) && !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "*";
        return makeLiveHTMLCollection(nullptr, fixed, tag);
    });
    b.def("getElementsByClassName", 1, [fixed](Value, std::span<const Value> a) {
        std::string cls = !a.empty() && !ev::isUndefined(a[0]) && !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
        return makeLiveHTMLCollection(nullptr, fixed, "." + cls);
    });
    b.def("getElementsByName", 1, [fixed](Value, std::span<const Value> a) {
        std::string name = !a.empty() && !ev::isUndefined(a[0]) && !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
        // getElementsByName is a live NodeList (HTML §3.1.5), not a collection.
        return makeLiveNodeList([fixed, name]() {
            std::vector<dom::Element*> els = runCollectionQuery(nullptr, fixed, "name:" + name);
            return std::vector<dom::Node*>(els.begin(), els.end());
        });
    });
    b.def("importNode", 2, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        if (a.empty()) return ev::null();
        dom::Node* node = hostNodeOf(a[0]);
        if (!node) return ev::null();
        bool deep = a.size() > 1 && ev::toBool(a[1]);
        dom::Node* cloned = doc->cloneNode(node, deep);
        return hostNodeValue(cloned);
    });
    b.def("adoptNode", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        if (a.empty()) return ev::null();
        dom::Node* node = hostNodeOf(a[0]);
        if (!node) return ev::null();
        dom::Node* adopted = doc->adoptNode(node);
        return hostNodeValue(adopted);
    });
    b.def("elementFromPoint", 2, [fixed](Value, std::span<const Value> a) {
        if (fixed) return ev::null();
        if (a.size() < 2) return ev::null();
        float x = static_cast<float>(ev::toDouble(a[0]));
        float y = static_cast<float>(ev::toDouble(a[1]));
        auto* e = hostEngine();
        if (!e) return ev::null();
        dom::Document* doc = documentFor(fixed);
        dom::Element* hit = hitTestClientPoint(e, doc, x, y);
        return wrapElement(hit);
    });
    b.def("elementsFromPoint", 2, [fixed](Value, std::span<const Value> a) {
        auto emptyArr = []() { return hostArrayOf(0, [](size_t) { return ev::undefined(); }); };
        if (fixed) return emptyArr();
        if (a.size() < 2) return emptyArr();
        float x = static_cast<float>(ev::toDouble(a[0]));
        float y = static_cast<float>(ev::toDouble(a[1]));
        auto* e = hostEngine();
        if (!e) return emptyArr();
        dom::Document* doc = documentFor(fixed);
        dom::Element* hit = hitTestClientPoint(e, doc, x, y);
        if (!hit) return emptyArr();
        std::vector<dom::Element*> chain;
        for (dom::Element* cur = hit; cur; cur = cur->parentElement()) {
            chain.push_back(cur);
        }
        return hostArrayOf(chain.size(), [&chain](size_t i) {
            return hostElementValue(chain[i]);
        });
    });
    b.accessor("defaultView", [](Value, std::span<const Value>) {
        ev::GlobalValue g = ev::globalValue("window");
        if (g.found) return g.value;
        g = ev::globalValue("globalThis");
        return g.found ? g.value : ev::null();
    }, nullptr);

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
    // The document's HTMLCollections (HTML §3.1.5 / DOM's children): live,
    // tree order, each filtering the elements under the root element.
    auto defDocCollection = [&b, fixed](const char* name, bool (*keep)(const dom::Element*)) {
        b.accessor(name,
                   [keep, fixed](Value, std::span<const Value>) {
                       return makeLiveCollection([keep, fixed]() {
                           std::vector<dom::Element*> out;
                           dom::Document* doc = documentFor(fixed);
                           if (doc && doc->documentElement()) collectWhere(doc->documentElement(), keep, out);
                           return out;
                       });
                   },
                   nullptr);
    };
    defDocCollection("forms", [](const dom::Element* e) { return isHtmlTag(e, "form"); });
    defDocCollection("images", [](const dom::Element* e) { return isHtmlTag(e, "img"); });
    defDocCollection("embeds", [](const dom::Element* e) { return isHtmlTag(e, "embed"); });
    defDocCollection("plugins", [](const dom::Element* e) { return isHtmlTag(e, "embed"); });
    defDocCollection("scripts", [](const dom::Element* e) { return isHtmlTag(e, "script"); });
    defDocCollection("links", [](const dom::Element* e) {
        return (isHtmlTag(e, "a") || isHtmlTag(e, "area")) && e->hasAttribute("href");
    });
    defDocCollection("anchors", [](const dom::Element* e) { return isHtmlTag(e, "a") && e->hasAttribute("name"); });
    b.accessor("children",
               [fixed](Value, std::span<const Value>) {
                   return makeLiveCollection([fixed]() {
                       std::vector<dom::Element*> out;
                       dom::Document* doc = documentFor(fixed);
                       if (doc && doc->documentElement()) out.push_back(doc->documentElement());
                       return out;
                   });
               },
               nullptr);
    // The element whose scrollTop/scrollLeft scroll the viewport: in a
    // standards-mode document (compatMode is always CSS1Compat) that is the
    // root element (CSSOM View), whose scroll offsets bro maps to the
    // viewport's (host_element_geometry.cpp).
    defDocElement("scrollingElement", &dom::Document::documentElement);
    b.accessor("head",
               [fixed](Value, std::span<const Value>) {
                   dom::Document* doc = documentFor(fixed);
                   if (!doc) return ev::null();
                   dom::Element* el = doc->querySelector("head");
                   return el ? hostElementValue(el) : ev::null();
               },
               nullptr);
    b.accessor("title",
               [fixed](Value, std::span<const Value>) -> Value {
                   dom::Document* doc = documentFor(fixed);
                   return doc ? ev::fromUtf8(doc->title()) : ev::fromUtf8("");
               },
               [fixed](Value, std::span<const Value> a) -> Value {
                   dom::Document* doc = documentFor(fixed);
                   if (doc && !a.empty()) {
                       doc->setTitle(ev::toUtf8(a[0]));
                   }
                   return ev::undefined();
               });
    b.accessor("readyState",
               [fixed](Value, std::span<const Value>) {
                   dom::Document* doc = documentFor(fixed);
                   return ev::fromUtf8(doc ? doc->readyState() : std::string("complete"));
               },
               nullptr);
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

    decorateDocumentExtras(b, fixed);

    const ElementSource root = [fixed]() -> dom::Element* {
        dom::Document* doc = documentFor(fixed);
        return doc ? doc->documentElement() : nullptr;
    };
    installElementEventTarget(b, root, "document");
    installDocumentEventHandlers(b, root);
    {
        ev::Persistent fonts(makeFontFaceSet([fixed]() { return documentFor(fixed); }));
        b.set("fonts", fonts.get());
    }
    decorateDocumentWebAnimations(b);
    // setPrototype allocates (the receiver goes to dictionary mode); the
    // value handed back is the post-call address, the one we return.
    return ev::setPrototype(b.get(), documentHostClass().prototype());
}

namespace {
std::unordered_map<dom::Document*, std::unique_ptr<ev::Persistent>> s_docWrappers;
}

// The app's document object (it follows the engine's document), as
// registered in the app's realm.
static ev::Persistent s_appDocumentValue;

void setAppDocumentValue(Value v) { s_appDocumentValue.set(v); }

ev::Persistent* hostDocumentWrapperSlot(dom::Document* doc) {
    if (!doc) return nullptr;
    auto it = s_docWrappers.find(doc);
    if (it == s_docWrappers.end() || !it->second) return nullptr;
    return it->second.get();
}

Value hostDocumentValue(dom::Document* doc) {
    if (!doc || !dom::Document::isLiveDocument(doc)) return ev::null();
    // Only to the code of the compartment that owns it: a document object's
    // methods act on its document whoever holds them, so another
    // compartment's is never handed out (null, as a cross-origin frame's
    // contentDocument is).
    if (compartmentForDocument(doc) != currentCompartment()) return ev::null();
    if (hostEngine() && doc == hostEngine()->document()) {
        if (ev::isObject(s_appDocumentValue.get())) return s_appDocumentValue.get();
        ev::GlobalValue g = ev::globalValue("document");
        if (g.found) return g.value;
    }
    hostSweepTouchDocument(doc);
    auto& slot = s_docWrappers[doc];
    if (!slot) slot = std::make_unique<ev::Persistent>();
    if (!ev::isUndefined(slot->get())) {
        return slot->get();
    }
    Value v = makeDocumentValue(doc);
    slot->set(v);
    return v;
}

void clearHostDocument(dom::Document* doc) {
    if (!doc) return;
    hostSweepForgetDocument(doc);
    s_docWrappers.erase(doc);
    removeHostMediaQueriesForDocument(doc);
}

void snapshotClassicScripts() {
    dom::Document* doc = currentHostDocument();
    if (doc) classicScriptSnapshots()[doc] = classicScriptElements(doc);
}

}  // namespace bro::bronze_host

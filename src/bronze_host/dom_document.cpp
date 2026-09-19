#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_range.h"
#include "bronze_host/host_selection.h"
#include "bronze_host/host_matchmedia.h"
#include "bronze_host/host_web_animations.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "platform/sdl_window.h"

#include "canvas/canvas_scene.h"
#include "layout/form_control.h"
#include "util/log.h"

#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "dom/node.h"

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

static dom::Document* s_activeDocOverride = nullptr;

namespace {

dom::Document* documentFor(dom::Document* fixed) {
    if (fixed) {
        fixed->setElementClonedCallback(&fireElementCloned);
        return fixed;
    }
    if (s_activeDocOverride) {
        s_activeDocOverride->setElementClonedCallback(&fireElementCloned);
        return s_activeDocOverride;
    }
    dom::Document* doc = hostEngine() ? hostEngine()->document() : nullptr;
    if (doc) doc->setElementClonedCallback(&fireElementCloned);
    return doc;
}

}  // namespace

dom::Document* currentHostDocument() {
    if (s_activeDocOverride) return s_activeDocOverride;
    return hostEngine() ? hostEngine()->document() : nullptr;
}

void setCurrentHostDocument(dom::Document* doc) {
    if (!doc || (hostEngine() && doc == hostEngine()->document())) {
        s_activeDocOverride = nullptr;
    } else {
        s_activeDocOverride = doc;
    }
}

namespace {

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
    std::string elTag = el->tagName();
    if (elTag.size() != selector.size()) return false;
    for (size_t i = 0; i < elTag.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(elTag[i])) !=
            std::tolower(static_cast<unsigned char>(selector[i]))) {
            return false;
        }
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
    HostProxyTraps traps;

    ObjectBuilder mb;
    mb.def("item", 1, [root, fixed, selector](Value, std::span<const Value> a) -> Value {
        int idx = a.empty() ? 0 : static_cast<int>(ev::toDouble(a[0]));
        auto elems = runCollectionQuery(root, fixed, selector);
        if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) return ev::null();
        return hostElementValue(elems[idx]);
    });
    mb.def("namedItem", 1, [root, fixed, selector](Value, std::span<const Value> a) -> Value {
        if (a.empty() || ev::isUndefined(a[0])) return ev::null();
        std::string name = ev::toUtf8(a[0]);
        auto elems = runCollectionQuery(root, fixed, selector);
        for (dom::Element* el : elems) {
            if (el->getAttribute("id") == name || el->getAttribute("name") == name) {
                return hostElementValue(el);
            }
        }
        return ev::null();
    });
    traps.methods = mb.get();

    traps.get = [root, fixed, selector](const std::string& key, Value& out) -> bool {
        if (key == "length") {
            auto elems = runCollectionQuery(root, fixed, selector);
            out = ev::fromDouble(static_cast<double>(elems.size()));
            return true;
        }
        bool isIndex = !key.empty();
        for (char c : key) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                isIndex = false;
                break;
            }
        }
        if (isIndex) {
            size_t idx = static_cast<size_t>(std::stoul(key));
            auto elems = runCollectionQuery(root, fixed, selector);
            if (idx < elems.size()) {
                out = hostElementValue(elems[idx]);
                return true;
            }
            out = ev::undefined();
            return true;
        }
        auto elems = runCollectionQuery(root, fixed, selector);
        for (dom::Element* el : elems) {
            if (el->getAttribute("id") == key || el->getAttribute("name") == key) {
                out = hostElementValue(el);
                return true;
            }
        }
        return false;
    };

    traps.has = [root, fixed, selector](const std::string& key) -> bool {
        if (key == "length") return true;
        bool isIndex = !key.empty();
        for (char c : key) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                isIndex = false;
                break;
            }
        }
        if (isIndex) {
            size_t idx = static_cast<size_t>(std::stoul(key));
            auto elems = runCollectionQuery(root, fixed, selector);
            return idx < elems.size();
        }
        auto elems = runCollectionQuery(root, fixed, selector);
        for (dom::Element* el : elems) {
            if (el->getAttribute("id") == key || el->getAttribute("name") == key) {
                return true;
            }
        }
        return false;
    };

    traps.ownKeys = [root, fixed, selector]() -> std::vector<std::string> {
        auto elems = runCollectionQuery(root, fixed, selector);
        std::vector<std::string> keys;
        keys.reserve(elems.size() + 1);
        for (size_t i = 0; i < elems.size(); ++i) {
            keys.push_back(std::to_string(i));
        }
        keys.push_back("length");
        return keys;
    };

    return makeHostProxy(std::move(traps));
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
    (void)fixed;
    b.accessor("hidden", [](Value, std::span<const Value>) {
        auto* e = hostEngine();
        return ev::fromBool(e ? !e->pageVisible() : false);
    }, nullptr);
    b.accessor("visibilityState", [](Value, std::span<const Value>) {
        auto* e = hostEngine();
        return ev::fromUtf8(e && !e->pageVisible() ? "hidden" : "visible");
    }, nullptr);
    b.accessor("location", [](Value, std::span<const Value>) {
        ev::GlobalValue loc = ev::globalValue("location");
        return loc.found ? loc.value : ev::null();
    }, nullptr);
    b.accessor("URL", [](Value, std::span<const Value>) { return ev::fromUtf8(locationHref()); }, nullptr);
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
        return makeLiveHTMLCollection(nullptr, fixed, "name:" + name);
    });
    b.def("importNode", 2, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::throwError("bronze host: engine has no document");
        if (a.empty()) return ev::null();
        dom::Node* node = hostNodeOf(a[0]);
        if (!node) return ev::null();
        bool deep = a.size() > 1 && ev::toBool(a[1]);
        dom::Node* cloned = doc->cloneNode(node, deep, /*preserveId=*/true);
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
        float vw = static_cast<float>(e->contentWidth());
        float vh = static_cast<float>(e->contentHeight());
        if (x < 0.0f || y < 0.0f || x >= vw || y >= vh) return ev::null();
        if (dom::Document* doc = e->document()) {
            e->flushLayoutForRead(doc);
        }
        float docX = x;
        float docY = y + e->viewportScrollY();
        dom::Element* hit = e->hitTest(docX, docY);
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
        float vw = static_cast<float>(e->contentWidth());
        float vh = static_cast<float>(e->contentHeight());
        if (x < 0.0f || y < 0.0f || x >= vw || y >= vh) return emptyArr();
        if (dom::Document* doc = e->document()) {
            e->flushLayoutForRead(doc);
        }
        float docX = x;
        float docY = y + e->viewportScrollY();
        dom::Element* hit = e->hitTest(docX, docY);
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
               [](Value, std::span<const Value>) {
                   auto* e = hostEngine();
                   if (!e) return ev::fromUtf8("complete");
                   return ev::fromUtf8(e->documentReadyState());
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

    installElementEventTarget(b, [fixed]() -> dom::Element* {
        dom::Document* doc = documentFor(fixed);
        return doc ? doc->documentElement() : nullptr;
    }, "document");
    decorateDocumentWebAnimations(b);
    // setPrototype allocates (the receiver goes to dictionary mode); the
    // value handed back is the post-call address, the one we return.
    return ev::setPrototype(b.get(), documentHostClass().prototype());
}

namespace {
std::unordered_map<dom::Document*, ev::Persistent> s_docWrappers;
}

Value hostDocumentValue(dom::Document* doc) {
    if (!doc) return ev::null();
    if (hostEngine() && doc == hostEngine()->document()) {
        ev::GlobalValue g = ev::globalValue("document");
        if (g.found) return g.value;
    }
    auto it = s_docWrappers.find(doc);
    if (it != s_docWrappers.end() && !ev::isUndefined(it->second.get())) {
        return it->second.get();
    }
    Value v = makeDocumentValue(doc);
    s_docWrappers[doc].set(v);
    return v;
}

void clearHostDocument(dom::Document* doc) {
    if (!doc) return;
    s_docWrappers.erase(doc);
    if (s_activeDocOverride == doc) {
        s_activeDocOverride = nullptr;
    }
    removeHostMediaQueriesForDocument(doc);
}

}  // namespace bro::bronze_host

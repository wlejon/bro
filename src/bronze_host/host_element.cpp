// The DOM an app *builds*, rather than the one it is handed.
//
// Core Element / HTMLElement handle registry, tree navigation, layout geometry,
// pointer capture, inline event handlers, and class registration.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"

#include "dom/document.h"
#include "dom/document_fragment.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "dom/event_target.h"
#include "dom/node.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

struct Registry {
    // unique_ptr so an entry's address is stable: every accessor on the wrapper
    // captures its HostNodeState*, and the map rehashes as the tree grows.
    std::vector<std::unique_ptr<HostNodeState>> entries;
    std::unordered_map<const dom::Node*, HostNodeState*> live;
    // Which documents we have asked to warn us. A set rather than the single
    // bool this was: the warning is per-document, and DOMParser makes a second
    // document reachable. With a bool, whichever document happened to own the
    // first node this layer ever wrapped was the only one being watched — and
    // if that was a parsed document, which never frees anything, the LIVE
    // document was left unwatched and every wrapper it handed out could outlive
    // its node.
    std::unordered_set<const dom::Document*> observed;
};

Registry& registry() {
    static Registry r;
    return r;
}

static dom::Element* s_fullscreenElement = nullptr;

// A doomed node's wrapper must stop answering BEFORE the storage goes away.
//
// The entry is dropped from the live map — so nothing can reach the dead
// Element* through us again, and an element later allocated at the same
// address gets a fresh entry rather than inheriting this one — and its
// Persistents are released here, which is a normal call site and therefore
// allowed to make embed calls. What is NOT done is freeing the entry: a
// wrapper the program still holds is a handle pointing at it, and that pointer
// has to stay valid. What it points at is now inert.
void onNodeFreed(dom::Document*, dom::Node* node) {
    if (s_fullscreenElement == node) {
        s_fullscreenElement = nullptr;
    }
    Registry& r = registry();
    auto it = r.live.find(node);
    if (it == r.live.end()) return;
    HostNodeState* st = it->second;
    r.live.erase(it);
    st->node = nullptr;
    st->el = nullptr;
    st->jsObj.set(ev::undefined());
    st->styleObj.set(ev::undefined());
    st->classListObj.set(ev::undefined());
    st->computedObj.set(ev::undefined());
    st->datasetObj.set(ev::undefined());
    st->inlineHandles.clear();
    st->inlineFns.clear();
    st->hasStyle = false;
    st->hasClassList = false;
    st->hasComputed = false;
    st->hasDataset = false;
}

// Takes a Node rather than an Element so text nodes, comments and fragments
// land in the SAME map as elements. One registry is what keeps identity a
// property of the node rather than of the kind of node: `parent.childNodes[0]
// === textNode` has to hold for the same reason `=== element` does, and a
// second map keyed on text nodes would be a second answer to the same question.
HostNodeState* stateFor(dom::Node* node) {
    if (!node) return nullptr;
    Registry& r = registry();
    auto it = r.live.find(node);
    if (it != r.live.end()) return it->second;
    if (dom::Document* doc = node->document()) {
        if (r.observed.insert(doc).second)
            doc->addNodeFreedObserver(&onNodeFreed);
    }
    auto owned = std::make_unique<HostNodeState>();
    HostNodeState* st = owned.get();
    st->node = node;
    st->el = node->nodeType() == dom::NodeType::Element
                 ? static_cast<dom::Element*>(node) : nullptr;
    r.entries.push_back(std::move(owned));
    r.live.emplace(node, st);
    return st;
}

}  // namespace

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

dom::AbsoluteRect borderBoxOf(dom::Element* el) {
    hostEngine()->flushLayoutForRead(el->document());
    return dom::absoluteBorderBox(el);
}

namespace {

Value makeRectValue(double x, double y, double w, double h) {
    ObjectBuilder r;
    r.set("x", ev::fromDouble(x));
    r.set("y", ev::fromDouble(y));
    r.set("left", ev::fromDouble(x));
    r.set("top", ev::fromDouble(y));
    r.set("right", ev::fromDouble(x + w));
    r.set("bottom", ev::fromDouble(y + h));
    r.set("width", ev::fromDouble(w));
    r.set("height", ev::fromDouble(h));
    return r.get();
}

dom::Element* siblingOf(dom::Element* el, int direction) {
    dom::Element* parent = el->parentElement();
    if (!parent) return nullptr;
    std::vector<dom::Element*> kids = parent->children();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] != el) continue;
        long long want = static_cast<long long>(i) + direction;
        if (want < 0 || want >= static_cast<long long>(kids.size())) return nullptr;
        return kids[static_cast<size_t>(want)];
    }
    return nullptr;
}

// The state behind a receiver.
// The state behind a receiver. Every member below reads this rather than
// closing over the pointer: one copy of each method lives on the prototype and
// serves every element, so the only way to know WHICH element is to ask the
// object the call arrived on.
HostNodeState* nodeStateOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* st = static_cast<HostNodeState*>(ev::handleData(v));
    if (st && st->tag == kHostElementTag) return st;
    Value idVal = ev::getProperty(v, "__bro_node_id__");
    if (ev::isNumber(idVal)) {
        auto* customSt = reinterpret_cast<HostNodeState*>(static_cast<uintptr_t>(ev::toDouble(idVal)));
        if (customSt && customSt->tag == kHostElementTag) return customSt;
    }
    return nullptr;
}

void installInlineEventHandler(ObjectBuilder& b, const char* propName, const char* eventType) {
    std::string type = eventType;
    b.accessor(
        propName,
        [type](Value self_, std::span<const Value>) {
            HostNodeState* st = nodeStateOf(self_);
            if (!st) return ev::null();
            auto it = st->inlineFns.find(type);
            if (it != st->inlineFns.end() && !ev::isUndefined(it->second.get())) {
                return it->second.get();
            }
            return ev::null();
        },
        [type](Value self_, std::span<const Value> a) {
            HostNodeState* st = nodeStateOf(self_);
            if (!st || !st->el) return ev::undefined();
            // Remove previous inline listener if set
            auto hIt = st->inlineHandles.find(type);
            if (hIt != st->inlineHandles.end() && hIt->second != 0) {
                st->el->removeEventListener(dom::ListenerHandle{hIt->second});
                st->inlineHandles.erase(hIt);
                st->inlineFns.erase(type);
            }
            Value fn = argAt(a, 0);
            if (ev::isFunction(fn)) {
                ev::Persistent fnP(fn);
                ev::Persistent self(self_);
                std::string origin = st->el->tagName() + " inline on" + type + " listener";
                dom::ListenerHandle handle = st->el->addEventListener(
                    type,
                    [fnP, self, origin](dom::Event& evt) {
                        callBronzeListener(fnP, self, evt, origin.c_str());
                    });
                if (handle) {
                    st->inlineHandles[type] = handle.id;
                    // insert_or_assign, not emplace: emplace KEEPS the existing
                    // value on a key collision, so any path that leaves a stale
                    // entry behind would hand the getter the previous function.
                    st->inlineFns.insert_or_assign(type, fnP);
                }
            }
            return ev::undefined();
        });
}

}  // namespace

void setHostFullscreenElement(dom::Element* el) {
    s_fullscreenElement = el;
}

dom::Element* hostFullscreenElement() {
    return s_fullscreenElement;
}

// ---------------------------------------------------------------------------
// The pieces other files in this layer use
// ---------------------------------------------------------------------------
Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make) {
    // THE ARRAY comes from parseJson, because the embed API has no createArray
    // and createObject makes a PLAIN object — no Array.prototype, no iterator.
    //
    // FILLING IT goes through Array.prototype.push, because embed::setElement
    // cannot: it is setProperty under a numeric-string key, and setProperty
    // refuses any receiver that is not a plain object (embed_object.cpp's
    // requirePlainObject) — an array is exactly what it refuses. push is a real
    // builtin reached through the same generic property read compiled code
    // uses, so this is the array's own append path rather than a poke at its
    // storage.
    //
    // One call per element rather than one call with `count` arguments: a
    // pre-built argument vector would be exactly the bug host_internal.h's GC
    // rule warns about, since every Value in it past the first allocation is
    // stale. `make(i)` runs with the array rooted and its result is pushed
    // immediately.
    ev::CallResult parsed = ev::parseJson("[]");
    if (parsed.thrown) {
        LOG_ERROR("bronze host: could not allocate an array");
        return ev::undefined();
    }
    ev::Persistent arr(parsed.value);
    if (count == 0) return arr.get();

    ev::Persistent push(ev::getProperty(arr.get(), "push"));
    if (!ev::isFunction(push.get())) {
        LOG_ERROR("bronze host: array has no push");
        return arr.get();
    }
    for (size_t i = 0; i < count; ++i) {
        Value v = make(i);
        ev::CallResult r = ev::call(push.get(), arr.get(),
                                    std::span<const Value>(&v, 1));
        if (r.thrown) {
            reportBronzeError("hostArrayOf", r.value);
            break;
        }
    }
    return arr.get();
}

dom::Element* hostElementOf(Value v) {
    HostNodeState* st = nodeStateOf(v);
    return st ? st->el : nullptr;
}

dom::Node* hostNodeOf(Value v) {
    HostNodeState* st = nodeStateOf(v);
    return st ? st->node : nullptr;
}

Value hostNodeValue(dom::Node* node) {
    if (!node) return ev::null();
    if (node->nodeType() == dom::NodeType::Element)
        return hostElementValue(static_cast<dom::Element*>(node));

    HostNodeState* st = stateFor(node);
    Value existing = st->jsObj.get();
    if (!ev::isUndefined(existing)) return existing;

    Value v;
    switch (node->nodeType()) {
        case dom::NodeType::Text:
        case dom::NodeType::Comment:
            v = makeCharacterDataValue(node);
            break;
        case dom::NodeType::DocumentFragment:
            v = makeFragmentValue(static_cast<dom::DocumentFragment*>(node));
            break;
        default:
            return ev::null();
    }
    // The make* call allocates, and allocation can grow the registry, so the
    // entry is re-fetched rather than reused across it.
    if (HostNodeState* again = stateFor(node)) again->jsObj.set(v);
    return v;
}

Value hostElementValue(dom::Element* el) {
    if (!el) return ev::null();
    HostNodeState* st = stateFor(el);
    Value existing = st->jsObj.get();
    if (!ev::isUndefined(existing)) return existing;
    // A canvas is more than an element — it owns a drawing buffer and a GL
    // context — so dom_globals.cpp builds that one, on top of this same core.
    Value v = isCanvasTag(el->tagName()) ? makeCanvasElementValue(el)
                                         : makePlainElementValue(el);
    // makeCanvas/makePlain allocate, and allocation can grow the registry, so
    // the entry is re-fetched rather than reused across the call.
    if (HostNodeState* again = stateFor(el)) again->jsObj.set(v);
    return v;
}

void noteHostElementValue(dom::Element* el, Value v) {
    if (HostNodeState* st = stateFor(el)) st->jsObj.set(v);
}

bool isCanvasTag(const std::string& tag) {
    return tag == "CANVAS" || tag == "canvas";
}

bool isImgTag(const std::string& tag) { return tag == "IMG" || tag == "img"; }

// Element is a real class: one prototype carrying the whole element surface,
// with every instance born on it. Before this, an element carried its own copy
// of all fifty-eight members — a thousand-element UI allocated fifty-eight
// thousand function objects to say the same fifty-eight things.
Value makeNodeHandleObject(dom::Node* node) {
    return nodeHostClass().make(stateFor(node), [](void*) {});
}

Value makeElementHandleObject(dom::Element* el) {
    Value tagProto = htmlInterfaceProto(el->tagName());
    return ev::makeHandle(stateFor(el), [](void*) {}, ev::Finalize::InSweep, tagProto);
}

HostNodeState* hostNodeStateFor(dom::Node* node) { return stateFor(node); }

HostNodeState* hostNodeStateOfValue(Value v) { return nodeStateOf(v); }

Value makePlainElementValue(dom::Element* el) {
    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);
    if (isImgTag(el->tagName())) primeImageFromMarkup(el);
    return b.get();
}

void installElementGlobals() {
    installHtmlInterfaces();
}

void installElementCore(ObjectBuilder& b, dom::Element* el) {
    b.set("nodeType", ev::fromDouble(1));
    b.set("tagName", ev::fromUtf8(el->tagName()));
    b.set("nodeName", ev::fromUtf8(el->tagName()));

}

void decorateElementProto(ObjectBuilder& b) {
    // The event-target trio, shared like everything else here. It used to be
    // the one part of the element surface that was per instance, because
    // installElementEventTarget took an ElementSource that captured a single
    // element; passing no source makes it read the receiver instead, so
    // `a.addEventListener === b.addEventListener` for any two elements — which
    // is what the web says and what `document.createElement('img')` has to
    // satisfy against `new Image()`.
    installElementEventTarget(b, nullptr, "Element");

    // One per event type the ENGINE actually dispatches to an element. A name
    // that is absent reads as `undefined` and an assignment to it goes nowhere,
    // which is the failure a library hits silently — so the list tracks
    // event_dispatch.cpp rather than a chosen subset of it.
    for (const auto& [prop, type] : {
             std::pair<const char*, const char*>{"onclick", "click"},
             {"ondblclick", "dblclick"},
             {"onmousedown", "mousedown"},
             {"onmouseup", "mouseup"},
             {"onmousemove", "mousemove"},
             {"onmouseover", "mouseover"},
             {"onmouseout", "mouseout"},
             {"onmouseenter", "mouseenter"},
             {"onmouseleave", "mouseleave"},
             {"oncontextmenu", "contextmenu"},
             {"onwheel", "wheel"},
             {"onpointerdown", "pointerdown"},
             {"onpointerup", "pointerup"},
             {"onpointermove", "pointermove"},
             {"onkeydown", "keydown"},
             {"onkeyup", "keyup"},
             {"onkeypress", "keypress"},
             {"oninput", "input"},
             {"onchange", "change"},
             {"onsubmit", "submit"},
             {"onscroll", "scroll"},
             {"onfocus", "focus"},
             {"onblur", "blur"},
         }) {
        installInlineEventHandler(b, prop, type);
    }

    b.accessor("id",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromUtf8(st->el ? st->el->id() : std::string());
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v))
                       st->el->setId(ev::isUndefined(v) ? "" : ev::toUtf8(v));
                   return ev::undefined();
               });
    b.accessor("className",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromUtf8(st->el ? st->el->className() : std::string());
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v))
                       st->el->setClassName(ev::isUndefined(v) ? "" : ev::toUtf8(v));
                   return ev::undefined();
               });
    b.accessor("textContent",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromUtf8(st->el ? st->el->textContent() : std::string());
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v))
                       st->el->setTextContent(ev::isUndefined(v) ? "" : ev::toUtf8(v));
                   return ev::undefined();
               });
    b.accessor("innerHTML",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromUtf8(st->el ? st->el->innerHTML() : std::string());
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v))
                       st->el->setInnerHTML(ev::isUndefined(v) ? "" : ev::toUtf8(v));
                   return ev::undefined();
               });

    b.accessor("contentEditable",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st || !st->el) return ev::fromUtf8("inherit");
                   if (!st->el->hasAttribute("contenteditable"))
                       return ev::fromUtf8("inherit");
                   std::string val = st->el->getAttribute("contenteditable");
                   if (val.empty() || val == "true")
                       return ev::fromUtf8("true");
                   return ev::fromUtf8(val);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st || !st->el) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v)) {
                       std::string val = ev::isUndefined(v) ? "" : ev::toUtf8(v);
                       st->el->setAttribute("contenteditable", val);
                   }
                   return ev::undefined();
               });

    b.accessor("isContentEditable",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st || !st->el) return ev::fromBool(false);
                   for (dom::Node* n = st->el; n; n = n->parentNode()) {
                       if (n->nodeType() == dom::NodeType::Element) {
                           auto* el = static_cast<dom::Element*>(n);
                           if (el->hasAttribute("contenteditable")) {
                               std::string val = el->getAttribute("contenteditable");
                               if (val == "false") return ev::fromBool(false);
                               return ev::fromBool(true);
                           }
                       }
                   }
                   return ev::fromBool(false);
               },
               nullptr);

    // ---- style, classList, dataset proxies --------------------------------
    decorateElementStyle(b);
    decorateElementDataset(b);

    // ---- attributes -------------------------------------------------------
    b.def("setAttribute", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        Value nameV = argAt(a, 0), valV = argAt(a, 1);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::undefined();
        std::string name = ev::toUtf8(nameV);
        std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV))
                              ? ev::toUtf8(valV) : "";
        std::string oldVal;
        bool had = st->el->hasAttribute(name);
        if (had) oldVal = st->el->getAttribute(name);
        st->el->setAttribute(name, val);
        onCustomElementAttributeChanged(st->el, name, had ? oldVal.c_str() : nullptr, val.c_str());
        return ev::undefined();
    });
    b.def("getAttribute", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        Value nameV = argAt(a, 0);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::null();
        std::string name = ev::toUtf8(nameV);
        if (!st->el->hasAttribute(name)) return ev::null();
        return ev::fromUtf8(st->el->getAttribute(name));
    });
    b.def("hasAttribute", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        Value nameV = argAt(a, 0);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::fromBool(false);
        return ev::fromBool(st->el->hasAttribute(ev::toUtf8(nameV)));
    });
    b.def("removeAttribute", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        Value nameV = argAt(a, 0);
        if (st->el && !ev::isObject(nameV) && !ev::isUndefined(nameV)) {
            std::string name = ev::toUtf8(nameV);
            if (st->el->hasAttribute(name)) {
                std::string oldVal = st->el->getAttribute(name);
                st->el->removeAttribute(name);
                onCustomElementAttributeChanged(st->el, name, oldVal.c_str(), nullptr);
            }
        }
        return ev::undefined();
    });
    b.def("toggleAttribute", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el || a.empty() || ev::isUndefined(a[0])) return ev::fromBool(false);
        std::string name = ev::toUtf8(a[0]);
        if (a.size() >= 2 && !ev::isUndefined(a[1])) {
            bool force = ev::toBool(a[1]);
            if (force) {
                st->el->setAttribute(name, "");
                return ev::fromBool(true);
            } else {
                st->el->removeAttribute(name);
                return ev::fromBool(false);
            }
        }
        if (st->el->hasAttribute(name)) {
            st->el->removeAttribute(name);
            return ev::fromBool(false);
        } else {
            st->el->setAttribute(name, "");
            return ev::fromBool(true);
        }
    });
    b.def("getAttributeNames", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        std::vector<std::string> names;
        for (const auto& [name, val] : st->el->attributes()) {
            names.push_back(name);
        }
        if (st->el->hasAttribute("style")) {
            names.push_back("style");
        }
        return hostArrayOf(names.size(), [&names](size_t i) {
            return ev::fromUtf8(names[i]);
        });
    });
    b.def("hasAttributes", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return ev::fromBool(false);
        return ev::fromBool(!st->el->attributes().empty() || st->el->hasAttribute("style"));
    });
    b.accessor("attributes",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st || !st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
                   std::vector<std::pair<std::string, std::string>> attrs;
                   for (const auto& [name, val] : st->el->attributes()) {
                       attrs.emplace_back(name, val);
                   }
                   if (st->el->hasAttribute("style")) {
                       attrs.emplace_back("style", st->el->getAttribute("style"));
                   }
                   return hostArrayOf(attrs.size(), [&attrs](size_t i) {
                       ObjectBuilder ab;
                       ab.set("name", ev::fromUtf8(attrs[i].first));
                       ab.set("value", ev::fromUtf8(attrs[i].second));
                       return ab.get();
                   });
               },
               nullptr);

    // ---- the tree ---------------------------------------------------------
    installNodeTree(b);
    decorateElementMutate(b);

    b.accessor("children",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) {
                       return hostArrayOf(0, [](size_t) { return ev::undefined(); });
                   }
                   std::vector<dom::Element*> kids = st->el->children();
                   return hostArrayOf(
                       kids.size(),
                       [&kids](size_t i) { return hostElementValue(kids[i]); });
               },
               nullptr);
    b.accessor("childElementCount",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(
                       st->el ? static_cast<double>(st->el->children().size()) : 0.0);
               },
               nullptr);

    // A Document reached as some node's parent answers null: there is no
    // wrapper for it here — `document` is a global built by dom_globals.cpp,
    // not a registry entry — and null is what the web answers for
    // parentElement at the root anyway.
    b.accessor("parentElement",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return st->el ? hostElementValue(st->el->parentElement())
                                 : ev::null();
               },
               nullptr);
    b.accessor("ownerDocument",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st || !st->node) return ev::null();
                   if (dom::Document* doc = st->node->document()) {
                       if (hostEngine() && doc == hostEngine()->document()) {
                           ev::GlobalValue g = ev::globalValue("document");
                           return g.found ? g.value : ev::null();
                       }
                       return hostDocumentValue(doc);
                   }
                   ev::GlobalValue g = ev::globalValue("document");
                   return g.found ? g.value : ev::null();
               },
               nullptr);

    auto defEdge = [&b](const char* name, bool first) {
        b.accessor(name,
                   [first](Value self_, std::span<const Value>) {
                       HostNodeState* st = nodeStateOf(self_);
                       if (!st || !st->el) return ev::null();
                       std::vector<dom::Element*> kids = st->el->children();
                       if (kids.empty()) return ev::null();
                       return hostElementValue(first ? kids.front() : kids.back());
                   },
                   nullptr);
    };
    defEdge("firstElementChild", true);
    defEdge("lastElementChild", false);

    auto defSibling = [&b](const char* name, int dir) {
        b.accessor(name,
                   [dir](Value self_, std::span<const Value>) {
                       HostNodeState* st = nodeStateOf(self_);
                       return st && st->el
                                  ? hostElementValue(siblingOf(st->el, dir))
                                  : ev::null();
                   },
                   nullptr);
    };
    defSibling("nextElementSibling", +1);
    defSibling("previousElementSibling", -1);

    b.def("querySelector", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        Value selV = argAt(a, 0);
        if (!st->el || ev::isObject(selV) || ev::isUndefined(selV)) return ev::null();
        return hostElementValue(st->el->querySelector(ev::toUtf8(selV)));
    });
    b.def("querySelectorAll", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        Value selV = argAt(a, 0);
        if (!st->el || ev::isObject(selV) || ev::isUndefined(selV))
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        std::vector<dom::Element*> found = st->el->querySelectorAll(ev::toUtf8(selV));
        return hostArrayOf(found.size(),
                           [&found](size_t i) { return hostElementValue(found[i]); });
    });
    b.def("matches", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el || a.empty() || ev::isUndefined(a[0])) return ev::fromBool(false);
        return ev::fromBool(st->el->matches(ev::toUtf8(a[0])));
    });
    b.def("closest", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el || a.empty() || ev::isUndefined(a[0])) return ev::null();
        dom::Element* found = st->el->closest(ev::toUtf8(a[0]));
        return found ? hostElementValue(found) : ev::null();
    });
    b.def("getElementsByTagName", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        std::string tag = a.empty() || ev::isUndefined(a[0]) ? "*" : ev::toUtf8(a[0]);
        return makeLiveHTMLCollection(st->el, st->el->document(), tag);
    });
    b.def("getElementsByClassName", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        std::string cls = a.empty() || ev::isUndefined(a[0]) ? "" : ev::toUtf8(a[0]);
        return makeLiveHTMLCollection(st->el, st->el->document(), "." + cls);
    });

    // ---- geometry ---------------------------------------------------------
    b.def("getBoundingClientRect", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st) return ev::undefined();
        if (!st->el) return makeRectValue(0, 0, 0, 0);
        dom::AbsoluteRect r = borderBoxOf(st->el);
        return makeRectValue(r.x, r.y, r.width, r.height);
    });
    b.accessor("clientWidth",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   hostEngine()->flushLayoutForRead(st->el->document());
                   return ev::fromDouble(st->el->layoutBox().contentRect.width);
               },
               nullptr);
    b.accessor("clientHeight",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   hostEngine()->flushLayoutForRead(st->el->document());
                   return ev::fromDouble(st->el->layoutBox().contentRect.height);
               },
               nullptr);
    b.accessor("offsetWidth",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? borderBoxOf(st->el).width : 0.0);
               },
               nullptr);
    b.accessor("offsetHeight",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? borderBoxOf(st->el).height : 0.0);
               },
               nullptr);
    // Document-absolute, not offset-parent-relative: what a UI positioning a
    // popup against an anchor wants, and what bro's own bindings answer.
    b.accessor("offsetLeft",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? borderBoxOf(st->el).x : 0.0);
               },
               nullptr);
    b.accessor("offsetTop",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? borderBoxOf(st->el).y : 0.0);
               },
               nullptr);
    b.accessor("scrollTop",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? st->el->scrollTopValue() : 0.0);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (st->el)
                       st->el->setScrollTopValue(
                           static_cast<float>(ev::toDouble(argAt(a, 0))));
                   return ev::undefined();
               });
    // bro's DOM tracks vertical scrolling only (dom::Element::scrollTop_), so
    // the horizontal half answers 0 and swallows a write it cannot honour —
    // the same answer the interpreted side gives, rather than a second story.
    b.accessor("scrollLeft",
               [](Value, std::span<const Value>) { return ev::fromDouble(0.0); },
               [](Value, std::span<const Value>) { return ev::undefined(); });
    b.accessor("scrollHeight",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   hostEngine()->flushLayoutForRead(st->el->document());
                   const auto& box = st->el->layoutBox();
                   return ev::fromDouble(
                       std::max(box.naturalHeight, box.contentRect.height));
               },
               nullptr);
    // scrollTo(x, y) and scrollTo({top, left}) are both written by real UI code.
    b.def("scrollTo", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return ev::undefined();
        if (!a.empty() && ev::isObject(a[0])) {
            Value top = ev::getProperty(a[0], "top");
            if (!ev::isUndefined(top))
                st->el->setScrollTopValue(static_cast<float>(ev::toDouble(top)));
        } else if (a.size() > 1) {
            st->el->setScrollTopValue(static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    // ---- pointer capture --------------------------------------------------
    auto pointerId = [](std::span<const Value> a) {
        return a.empty() || ev::isUndefined(a[0]) ? engine::Engine::kMousePointerId
                                                  : i32At(a, 0);
    };
    b.def("setPointerCapture", 1, [pointerId](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (st && st->el) hostEngine()->setPointerCapture(st->el, pointerId(a));
        return ev::undefined();
    });
    b.def("releasePointerCapture", 1,
          [pointerId](Value self_, std::span<const Value> a) {
              HostNodeState* st = nodeStateOf(self_);
              if (st && st->el) hostEngine()->releasePointerCapture(st->el, pointerId(a));
              return ev::undefined();
          });
    b.def("hasPointerCapture", 1, [pointerId](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return ev::fromBool(false);
        return ev::fromBool(hostEngine()->hasPointerCapture(st->el, pointerId(a)));
    });

    // ---- pointer lock -----------------------------------------------------
    b.def("requestPointerLock", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (st && st->el) {
            if (auto* e = hostEngine()) {
                e->requestPointerLock(st->el);
            }
        }
        return ev::undefined();
    });

    // ---- fullscreen -------------------------------------------------------
    b.def("requestFullscreen", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (st && st->el) {
            setHostFullscreenElement(st->el);
        }
        if (auto* e = hostEngine()) {
            e->setFullscreenState(true);
            if (auto* win = e->window()) {
                win->setFullscreen(true);
            }
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // ---- focus & blur -----------------------------------------------------
    // ---- focus ------------------------------------------------------------
    b.def("focus", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Document* doc = st->el->document();
        if (!doc) return ev::undefined();
        dom::Element* prev = doc->activeElement();
        if (prev == st->el) return ev::undefined();
        if (auto* eng = hostEngine()) eng->handleProgrammaticFocus(doc, prev, st->el);
        doc->setActiveElement(st->el);

        if (prev) {
            dom::FocusEvent blurEvt("blur", false, false);
            blurEvt.setRelatedTarget(st->el);
            dom::dispatchDomEvent(prev, blurEvt);
        }
        {
            dom::FocusEvent focusEvt("focus", false, false);
            focusEvt.setRelatedTarget(prev);
            dom::dispatchDomEvent(st->el, focusEvt);
        }
        if (prev) {
            dom::FocusEvent focusoutEvt("focusout", true, false);
            focusoutEvt.setRelatedTarget(st->el);
            dom::dispatchDomEvent(prev, focusoutEvt);
        }
        {
            dom::FocusEvent focusinEvt("focusin", true, false);
            focusinEvt.setRelatedTarget(prev);
            dom::dispatchDomEvent(st->el, focusinEvt);
        }
        return ev::undefined();
    });
    b.def("blur", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Document* doc = st->el->document();
        if (!doc || doc->activeElement() != st->el) return ev::undefined();
        if (auto* eng = hostEngine()) eng->handleProgrammaticFocus(doc, st->el, nullptr);
        doc->setActiveElement(nullptr);

        {
            dom::FocusEvent blurEvt("blur", false, false);
            blurEvt.setRelatedTarget(nullptr);
            dom::dispatchDomEvent(st->el, blurEvt);
        }
        {
            dom::FocusEvent focusoutEvt("focusout", true, false);
            focusoutEvt.setRelatedTarget(nullptr);
            dom::dispatchDomEvent(st->el, focusoutEvt);
        }
        return ev::undefined();
    });

    // ---- form controls ----------------------------------------------------
    decorateElementForms(b);
}

}  // namespace bro::bronze_host

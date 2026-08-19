// The DOM an app *builds*, rather than the one it is handed.
//
// Core Element / HTMLElement handle registry, tree navigation, layout geometry,
// pointer capture, inline event handlers, and class registration.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/document.h"
#include "dom/document_fragment.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
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
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

struct Registry {
    std::vector<std::unique_ptr<HostNodeState>> entries;
    std::unordered_map<const dom::Node*, HostNodeState*> live;
    std::unordered_set<const dom::Document*> observed;
};

Registry& registry() {
    static Registry r;
    return r;
}

static dom::Element* s_fullscreenElement = nullptr;

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

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

dom::AbsoluteRect borderBoxOf(dom::Element* el) {
    hostEngine()->flushLayoutForRead(el->document());
    return dom::absoluteBorderBox(el);
}

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
HostNodeState* nodeStateOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* st = static_cast<HostNodeState*>(ev::handleData(v));
    if (!st || st->tag != kHostElementTag) return nullptr;
    return st;
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
                    st->inlineFns.emplace(type, fnP);
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

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make) {
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
    if (!ev::isObject(v)) return nullptr;
    auto* st = static_cast<HostNodeState*>(ev::handleData(v));
    if (!st || st->tag != kHostElementTag) return nullptr;
    return st->el;
}

dom::Node* hostNodeOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* st = static_cast<HostNodeState*>(ev::handleData(v));
    if (!st || st->tag != kHostElementTag) return nullptr;
    return st->node;
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
    if (HostNodeState* again = stateFor(node)) again->jsObj.set(v);
    return v;
}

Value hostElementValue(dom::Element* el) {
    if (!el) return ev::null();
    HostNodeState* st = stateFor(el);
    Value existing = st->jsObj.get();
    if (!ev::isUndefined(existing)) return existing;
    Value v = isCanvasTag(el->tagName()) ? makeCanvasElementValue(el)
                                         : makePlainElementValue(el);
    if (HostNodeState* again = stateFor(el)) again->jsObj.set(v);
    return v;
}

void noteHostElementValue(dom::Element* el, Value v) {
    if (HostNodeState* st = stateFor(el)) st->jsObj.set(v);
}

bool isCanvasTag(const std::string& tag) {
    return tag == "CANVAS" || tag == "canvas";
}

HostClass g_elementClass;

Value makeNodeHandleObject(dom::Node* node) {
    return ev::makeHandle(stateFor(node), [](void*) {});
}

Value makeElementHandleObject(dom::Element* el) {
    return g_elementClass.make(stateFor(el), [](void*) {});
}

HostNodeState* hostNodeStateFor(dom::Node* node) { return stateFor(node); }

HostNodeState* hostNodeStateOfValue(Value v) { return nodeStateOf(v); }

Value makePlainElementValue(dom::Element* el) {
    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);
    return b.get();
}

void decorateElementProto(ObjectBuilder& b);

void installElementGlobals() {
    g_elementClass.install("Element", 0, nullptr, decorateElementProto);
    g_elementClass.alias("HTMLElement");
}

void installElementCore(ObjectBuilder& b, dom::Element* el) {
    b.set("nodeType", ev::fromDouble(1));
    b.set("tagName", ev::fromUtf8(el->tagName()));
    b.set("nodeName", ev::fromUtf8(el->tagName()));

    installElementEventTarget(b, [st = stateFor(el)]() { return st->el; },
                               el->tagName().c_str());
}

void decorateElementProto(ObjectBuilder& b) {
    installInlineEventHandler(b, "onclick", "click");
    installInlineEventHandler(b, "onmousedown", "mousedown");
    installInlineEventHandler(b, "onmouseup", "mouseup");
    installInlineEventHandler(b, "onmousemove", "mousemove");
    installInlineEventHandler(b, "onkeydown", "keydown");
    installInlineEventHandler(b, "onkeyup", "keyup");
    installInlineEventHandler(b, "oninput", "input");
    installInlineEventHandler(b, "onchange", "change");
    installInlineEventHandler(b, "onfocus", "focus");
    installInlineEventHandler(b, "onblur", "blur");

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
        std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV))
                              ? ev::toUtf8(valV) : "";
        st->el->setAttribute(ev::toUtf8(nameV), val);
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
        if (st->el && !ev::isObject(nameV) && !ev::isUndefined(nameV))
            st->el->removeAttribute(ev::toUtf8(nameV));
        return ev::undefined();
    });

    // ---- the tree ---------------------------------------------------------
    installNodeTree(b);

    b.def("append", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = nodeStateOf(self_);
        if (!st || !st->el) return ev::undefined();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(st->el, child, nullptr);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->el->document())
                    st->el->appendChild(doc->createTextNode(ev::toUtf8(v)));
            }
        }
        return ev::undefined();
    });

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

    b.accessor("parentElement",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return st->el ? hostElementValue(st->el->parentElement())
                                 : ev::null();
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
    b.def("focus", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (st && st->el)
            if (dom::Document* doc = st->el->document()) doc->setActiveElement(st->el);
        return ev::undefined();
    });
    b.def("blur", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = nodeStateOf(self_);
        if (st && st->el)
            if (dom::Document* doc = st->el->document())
                if (doc->activeElement() == st->el) doc->setActiveElement(nullptr);
        return ev::undefined();
    });

    // ---- form controls ----------------------------------------------------
    decorateElementForms(b);
}

}  // namespace bro::bronze_host

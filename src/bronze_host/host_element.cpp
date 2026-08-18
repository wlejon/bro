// The DOM an app *builds*, rather than the one it is handed.
//
// dom_globals.cpp's element wrapper was shaped by what a WebGL app needs: one
// canvas, maybe an overlay div, a handful of style properties written once at
// startup, and nothing ever read back. An app with a user interface is the
// other shape entirely — it builds a tree hundreds of elements deep and then
// reads it back: an element's children, a class it may or may not carry, the
// width layout gave it. This file is that surface, and it is the half that was
// missing.
//
// Four things here are not just more bindings.
//
// THE REGISTRY. Identity is load-bearing in a UI — `event.target === this.dom`
// is how a widget decides an event is its own — so an element must answer as
// the SAME value however it is reached: through `children`, through
// `querySelector`, through an event target. dom_globals.cpp got that by
// scanning its canvas list, which is one entry long. A tree needs a map, and a
// map of raw Element* needs to be told when a node dies — hence
// Document::addNodeFreedObserver, and hence this being the only file that turns
// a dom::Element* into a Value.
//
// THE HANDLE. `parent.appendChild(child)` has to recover a dom::Element* from
// the value the program is holding, so the wrapper IS an embed handle whose
// data is its registry entry. The entry is never freed from the handle's
// finalizer: it holds Persistents, and ~Persistent is an embed call, which
// host_internal.h's GC rule forbids a finalizer from making. A retired entry
// keeps its ~50 bytes and answers inert instead.
//
// REAL ARRAYS. `children` and `querySelectorAll` get iterated — `for…of`,
// `Array.from`, `.map`. An object carrying numeric keys and a `length` has none
// of Array.prototype and no iterator, so each of those is a TypeError at the
// call site rather than an empty result. The embed API has no createArray, but
// it has parseJson, and JSON arrays are real arrays.
//
// LAZY SUB-OBJECTS. `style`, `classList` and `dataset` are an object per
// element, and a UI makes thousands of elements. Each is built on first read
// and cached in the registry entry, so an element nobody styles costs nothing.
// `style`, `dataset` and the computed declaration are PROXIES (host_proxy.cpp)
// rather than accessor sets: their keys are not known when the object is built,
// which is what used to cap `style` at a curated ~110 properties and rule out
// `dataset` altogether.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/document.h"
#include "dom/document_fragment.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/node.h"
#include "dom/style_proxy.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "js/dom_bindings.h"
#include "layout/computed_style.h"
#include "layout/form_control.h"
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

// HostNodeState is in host_internal.h: host_node.cpp builds the text, comment
// and fragment wrappers on the same entries, so the registry is the layer's,
// not this file's. What stays here is the registry itself.
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

// ---------------------------------------------------------------------------
// style
// ---------------------------------------------------------------------------

// `el.style.backgroundColor` is one property name in JS and another in CSS.
const std::string& dashedFor(const char* camel) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(camel);
    if (it != cache.end()) return it->second;
    std::string out;
    for (const char* p = camel; *p; ++p) {
        if (std::isupper(static_cast<unsigned char>(*p))) {
            out += '-';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        } else {
            out += *p;
        }
    }
    return cache.emplace(camel, std::move(out)).first->second;
}

// No longer the set of properties that WORK — the proxy below reaches all 363
// and any custom `--*` besides. What survives of this list is the one job a
// trap cannot do: enumerating a computed declaration, where the web lists
// every supported property and htmlayout has no registry to ask for that list.
// It is the set real apps assign — what three.js, pixi and the three.js
// editor's widget library write.
const char* const kStyleProps[] = {
    "display", "position", "top", "right", "bottom", "left", "float", "clear",
    "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "boxSizing", "overflow", "overflowX", "overflowY", "resize", "zIndex",
    "border", "borderTop", "borderRight", "borderBottom", "borderLeft",
    "borderWidth", "borderStyle", "borderColor", "borderRadius",
    "borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth",
    "outline", "outlineColor", "outlineWidth", "outlineStyle",
    "flex", "flexDirection", "flexWrap", "flexGrow", "flexShrink", "flexBasis",
    "alignItems", "alignSelf", "alignContent", "justifyContent", "justifySelf",
    "gap", "rowGap", "columnGap", "order",
    "gridTemplateColumns", "gridTemplateRows", "gridColumn", "gridRow",
    "color", "background", "backgroundColor", "backgroundImage",
    "backgroundSize", "backgroundPosition", "backgroundRepeat",
    "opacity", "visibility", "filter", "boxShadow", "mixBlendMode",
    "font", "fontFamily", "fontSize", "fontWeight", "fontStyle", "fontVariant",
    "lineHeight", "letterSpacing", "wordSpacing", "textAlign", "textDecoration",
    "textIndent", "textOverflow", "textShadow", "textTransform", "textRendering",
    "whiteSpace", "wordBreak", "overflowWrap", "verticalAlign", "direction",
    "cursor", "pointerEvents", "userSelect", "touchAction", "transform",
    "transformOrigin", "transition", "animation", "willChange",
    "objectFit", "objectPosition", "listStyle", "borderCollapse", "tableLayout",
};

// The name an app wrote, in the spelling StyleProxy stores. Both are legal JS
// — `style.backgroundColor` and `style['background-color']` name one property
// on the web — and a custom property (`--brand`) is neither, so it passes
// through untouched: kebab-casing it would turn `--fooBar` into `--foo-bar`.
std::string styleKeyToCss(const std::string& key) {
    if (key.size() >= 2 && key[0] == '-' && key[1] == '-') return key;
    return dom::StyleProxy::camelToKebab(key);
}

Value makeStyleObject(HostNodeState* st) {
    ObjectBuilder b;
    b.def("setProperty", 2, [st](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0), valV = argAt(a, 1);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::undefined();
        std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV))
                              ? ev::toUtf8(valV) : "";
        st->el->style().setProperty(ev::toUtf8(nameV), val);
        return ev::undefined();
    });
    b.def("getPropertyValue", 1, [st](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::fromUtf8("");
        return ev::fromUtf8(st->el->style().getProperty(ev::toUtf8(nameV)));
    });
    b.def("removeProperty", 1, [st](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::fromUtf8("");
        std::string name = ev::toUtf8(nameV);
        std::string old = st->el->style().getProperty(name);
        st->el->style().removeProperty(name);
        return ev::fromUtf8(old);
    });
    b.accessor("cssText",
               [st](Value, std::span<const Value>) {
                   if (!st->el) return ev::fromUtf8("");
                   return ev::fromUtf8(st->el->style().cssText());
               },
               [st](Value, std::span<const Value> a) {
                   Value v = argAt(a, 0);
                   if (!st->el || ev::isObject(v)) return ev::undefined();
                   st->el->style().setCssText(ev::isUndefined(v) ? "" : ev::toUtf8(v));
                   return ev::undefined();
               });

    HostProxyTraps t;
    t.methods = b.get();
    // Every CSS property is a property of this object, set or not — that is
    // what makes `style.gridAutoFlow` readable without a table saying so, and
    // it is also what the web does: an unset property reads "".
    t.get = [st](const std::string& key, Value& out) {
        if (!st->el) return false;
        out = ev::fromUtf8(st->el->style().getProperty(styleKeyToCss(key)));
        return true;
    };
    t.set = [st](const std::string& key, Value v) {
        if (!st->el || ev::isObject(v)) return;
        const std::string css = styleKeyToCss(key);
        // null and undefined REMOVE, matching the accessor pairs this
        // replaced; the web instead stringifies, but a UI that clears a
        // property by assigning null is common enough that the older
        // behaviour is the one worth keeping.
        if (ev::isUndefined(v) || ev::isNull(v)) st->el->style().removeProperty(css);
        else st->el->style().setProperty(css, ev::toUtf8(v));
    };
    // Membership and enumeration answer for the SET properties only. `'color'
    // in el.style` being false on a bare element is what the web says too —
    // the declaration owns only what was assigned to it — and it is what keeps
    // `Object.keys(el.style)` the short useful list rather than all 363.
    t.has = [st](const std::string& key) {
        if (!st->el) return false;
        return !st->el->style().getProperty(styleKeyToCss(key)).empty();
    };
    t.ownKeys = [st]() {
        std::vector<std::string> keys;
        if (!st->el) return keys;
        for (const auto& [name, value] : st->el->style().properties()) {
            (void)value;
            keys.push_back(name);
        }
        return keys;
    };
    t.remove = [st](const std::string& key) {
        if (st->el) st->el->style().removeProperty(styleKeyToCss(key));
    };
    return makeHostProxy(std::move(t));
}

// ---------------------------------------------------------------------------
// Computed style
// ---------------------------------------------------------------------------

// What getComputedStyle() answers: the RESOLVED value of each property — used
// widths off the layout box, lengths absolutised to px, colours serialised to
// rgb() — rather than whatever string the author wrote. The resolution itself
// is layout::computedProperty, shared with bro's own JS bindings, because two
// binding layers over one engine must not disagree about what an element's
// computed width is.
//
// Read-only in both directions: the web's computed CSSStyleDeclaration ignores
// writes, and so do these accessors (no setter at all, which makes an assignment
// a silent no-op exactly as it is there).
//
// Layout is flushed on every read rather than once when the object is built.
// The object is LIVE — the web hands back a declaration that keeps tracking the
// element — so a UI that reads a width, changes a class and reads again must
// see the second answer, and a snapshot taken at construction would hand back
// the first.
Value makeComputedStyleObject(HostNodeState* st) {
    ObjectBuilder b;
    auto resolve = [st](const std::string& css) {
        if (!st->el) return std::string();
        hostEngine()->flushLayoutForRead(st->el->document());
        return layout::computedProperty(st->el, css, hostEngine()->textMetrics());
    };
    b.def("getPropertyValue", 1, [resolve](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (ev::isObject(nameV) || ev::isUndefined(nameV)) return ev::fromUtf8("");
        return ev::fromUtf8(resolve(ev::toUtf8(nameV)));
    });
    // Present and inert, because that is what they are on the web's computed
    // declaration — a UI that calls them on the wrong object gets the web's
    // behaviour rather than a TypeError from a missing method.
    b.def("setProperty", 2, [](Value, std::span<const Value>) {
        return ev::undefined();
    });
    b.def("removeProperty", 1, [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
    });

    HostProxyTraps t;
    t.methods = b.get();
    t.get = [resolve](const std::string& key, Value& out) {
        out = ev::fromUtf8(resolve(styleKeyToCss(key)));
        return true;
    };
    // No `set` and no `remove`: the web's computed declaration ignores both,
    // and leaving the callbacks empty is how this file spells that.
    t.has = [resolve](const std::string& key) {
        return !resolve(styleKeyToCss(key)).empty();
    };
    // Enumeration is the one place the curated list still earns its keep. The
    // web enumerates every supported property; htmlayout has no registry to
    // ask for that list, and resolving all 363 to find the non-empty ones
    // would run a layout-flushed resolve per property per enumeration. This
    // list is what an app that enumerates a computed style is looking for.
    t.ownKeys = [resolve]() {
        std::vector<std::string> keys;
        for (const char* camel : kStyleProps) {
            const std::string& css = dashedFor(camel);
            if (!resolve(css).empty()) keys.push_back(css);
        }
        return keys;
    };
    return makeHostProxy(std::move(t));
}

// ---------------------------------------------------------------------------
// classList
// ---------------------------------------------------------------------------

std::vector<std::string> classesOf(dom::Element* el) {
    std::vector<std::string> out;
    if (!el) return out;
    const std::string& s = el->className();
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

void writeClasses(dom::Element* el, const std::vector<std::string>& list) {
    std::string s;
    for (const std::string& c : list) {
        if (!s.empty()) s += ' ';
        s += c;
    }
    el->setClassName(s);
}

bool hasClass(dom::Element* el, const std::string& name) {
    for (const std::string& c : classesOf(el))
        if (c == name) return true;
    return false;
}

// ---------------------------------------------------------------------------
// dataset
// ---------------------------------------------------------------------------

// A view over the element's `data-*` attributes, and the surface this layer
// refused to fake: its keys are whatever attributes exist plus whatever the
// app invents, so nothing short of a property trap can express it, and a
// `dataset` that dropped `el.dataset.k = 'v'` would be worse than none. It is
// a pure view — no state of its own, every read and write going straight to
// the attribute map — so it stays correct across `setAttribute('data-k', …)`
// from the other direction, which a snapshot object would not.
//
// The name mapping is the web's: `data-user-id` <-> `dataset.userId`. An
// attribute whose name has an uppercase letter cannot be produced by that map
// and so is not reachable here, exactly as on the web.
std::string datasetKeyToAttr(const std::string& key) {
    return "data-" + dom::StyleProxy::camelToKebab(key);
}

bool datasetAttrToKey(const std::string& attr, std::string* out) {
    if (attr.size() <= 5 || attr.compare(0, 5, "data-") != 0) return false;
    *out = dom::StyleProxy::kebabToCamel(attr.substr(5));
    return true;
}

Value makeDatasetObject(HostNodeState* st) {
    HostProxyTraps t;
    t.get = [st](const std::string& key, Value& out) {
        if (!st->el) return false;
        const std::string attr = datasetKeyToAttr(key);
        // Absent answers undefined rather than "", which is what the web does
        // and what `if (el.dataset.role)` depends on.
        if (!st->el->hasAttribute(attr)) return false;
        out = ev::fromUtf8(st->el->getAttribute(attr));
        return true;
    };
    t.set = [st](const std::string& key, Value v) {
        if (!st->el || ev::isObject(v)) return;
        st->el->setAttribute(datasetKeyToAttr(key),
                             ev::isUndefined(v) ? "undefined" : ev::toUtf8(v));
    };
    t.has = [st](const std::string& key) {
        return st->el && st->el->hasAttribute(datasetKeyToAttr(key));
    };
    t.ownKeys = [st]() {
        std::vector<std::string> keys;
        if (!st->el) return keys;
        for (const auto& [name, value] : st->el->attributes()) {
            (void)value;
            std::string key;
            if (datasetAttrToKey(name, &key)) keys.push_back(key);
        }
        return keys;
    };
    t.remove = [st](const std::string& key) {
        if (st->el) st->el->removeAttribute(datasetKeyToAttr(key));
    };
    return makeHostProxy(std::move(t));
}

// Every mutator takes a variadic list, as DOMTokenList does: `classList.add(a, b)`
// is one call on the web and widget libraries write it that way.
Value makeClassListObject(HostNodeState* st) {
    ObjectBuilder b;
    b.def("add", 1, [st](Value, std::span<const Value> a) {
        if (!st->el) return ev::undefined();
        std::vector<std::string> list = classesOf(st->el);
        bool changed = false;
        for (const Value& v : a) {
            if (ev::isObject(v) || ev::isUndefined(v)) continue;
            std::string name = ev::toUtf8(v);
            if (name.empty()) continue;
            if (std::find(list.begin(), list.end(), name) == list.end()) {
                list.push_back(name);
                changed = true;
            }
        }
        if (changed) writeClasses(st->el, list);
        return ev::undefined();
    });
    b.def("remove", 1, [st](Value, std::span<const Value> a) {
        if (!st->el) return ev::undefined();
        std::vector<std::string> list = classesOf(st->el);
        size_t before = list.size();
        for (const Value& v : a) {
            if (ev::isObject(v) || ev::isUndefined(v)) continue;
            std::string name = ev::toUtf8(v);
            list.erase(std::remove(list.begin(), list.end(), name), list.end());
        }
        if (list.size() != before) writeClasses(st->el, list);
        return ev::undefined();
    });
    b.def("contains", 1, [st](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        if (!st->el || ev::isObject(v) || ev::isUndefined(v))
            return ev::fromBool(false);
        return ev::fromBool(hasClass(st->el, ev::toUtf8(v)));
    });
    // The two-argument form is the one a UI actually calls —
    // `dom.classList.toggle('active', isActive)` — where the force argument
    // decides and the current state does not.
    b.def("toggle", 2, [st](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        if (!st->el || ev::isObject(v) || ev::isUndefined(v))
            return ev::fromBool(false);
        std::string name = ev::toUtf8(v);
        bool present = hasClass(st->el, name);
        bool want = (a.size() > 1 && !ev::isUndefined(a[1])) ? ev::toBool(a[1])
                                                            : !present;
        if (want == present) return ev::fromBool(want);
        std::vector<std::string> list = classesOf(st->el);
        if (want) list.push_back(name);
        else list.erase(std::remove(list.begin(), list.end(), name), list.end());
        writeClasses(st->el, list);
        return ev::fromBool(want);
    });
    b.def("replace", 2, [st](Value, std::span<const Value> a) {
        Value oldV = argAt(a, 0), newV = argAt(a, 1);
        if (!st->el || ev::isObject(oldV) || ev::isObject(newV))
            return ev::fromBool(false);
        std::string oldName = ev::toUtf8(oldV), newName = ev::toUtf8(newV);
        std::vector<std::string> list = classesOf(st->el);
        auto it = std::find(list.begin(), list.end(), oldName);
        if (it == list.end()) return ev::fromBool(false);
        *it = newName;
        writeClasses(st->el, list);
        return ev::fromBool(true);
    });
    b.def("item", 1, [st](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        std::vector<std::string> list = classesOf(st->el);
        if (idx < 0 || static_cast<size_t>(idx) >= list.size()) return ev::null();
        return ev::fromUtf8(list[static_cast<size_t>(idx)]);
    });
    b.accessor("length",
               [st](Value, std::span<const Value>) {
                   return ev::fromDouble(
                       static_cast<double>(classesOf(st->el).size()));
               },
               nullptr);
    b.accessor("value",
               [st](Value, std::span<const Value>) {
                   return ev::fromUtf8(st->el ? st->el->className() : std::string());
               },
               [st](Value, std::span<const Value> a) {
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v) && !ev::isUndefined(v))
                       st->el->setClassName(ev::toUtf8(v));
                   return ev::undefined();
               });
    return b.get();
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

// Every geometry read flushes layout first, for the reason bro's own bindings
// do (Engine::flushLayoutForRead): an element appended and measured in one turn
// must measure the box it now has, not the one it had before the append.
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
            // A Document reached as some node's parent. There is no wrapper for
            // it here — `document` is a global built by dom_globals.cpp, not a
            // registry entry — and answering null is what the web does for
            // parentElement at the root anyway.
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

Value hostComputedStyleFor(Value elValue) {
    HostNodeState* st = nullptr;
    if (ev::isObject(elValue)) {
        auto* candidate = static_cast<HostNodeState*>(ev::handleData(elValue));
        if (candidate && candidate->tag == kHostElementTag && candidate->el)
            st = candidate;
    }
    if (!st) {
        // getComputedStyle(somethingElse). The web throws; bro's own bindings
        // answer an object whose every property is the empty string, and the
        // two surfaces over one engine agree rather than one of them being
        // stricter. A UI probing an optional element gets "" either way.
        ObjectBuilder empty;
        empty.def("getPropertyValue", 1, [](Value, std::span<const Value>) {
            return ev::fromUtf8("");
        });
        return empty.get();
    }
    if (!st->hasComputed) {
        Value c = makeComputedStyleObject(st);
        // makeComputedStyleObject allocates, so the entry pointer is re-taken
        // from the value rather than trusted across the call.
        st = static_cast<HostNodeState*>(ev::handleData(elValue));
        st->computedObj.set(c);
        st->hasComputed = true;
    }
    return st->computedObj.get();
}

bool isCanvasTag(const std::string& tag) {
    return tag == "CANVAS" || tag == "canvas";
}

// The state behind a receiver. Every member below reads this rather than
// closing over the pointer: one copy of each method lives on the prototype and
// serves every element, so the only way to know WHICH element is to ask the
// object the call arrived on.
HostNodeState* nodeStateOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* st = static_cast<HostNodeState*>(ev::handleData(v));
    if (!st || st->tag != kHostElementTag) return nullptr;
    return st;
}

// Element is a real class: one prototype carrying the whole element surface,
// with every instance born on it. Before this, an element carried its own copy
// of all fifty-eight members — a thousand-element UI allocated fifty-eight
// thousand function objects to say the same fifty-eight things.
HostClass g_elementClass;

Value makeNodeHandleObject(dom::Node* node) {
    return ev::makeHandle(stateFor(node), [](void*) {
        // Deliberately empty. The entry belongs to the registry, and freeing it
        // here would destroy its Persistents from inside a finalizer — the one
        // thing host_internal.h's GC rule forbids.
    });
}

Value makeElementHandleObject(dom::Element* el) {
    // Born on Element.prototype. makeNodeHandleObject stays bare: host_node.cpp
    // builds Text, Comment and DocumentFragment through it, and none of those
    // is an Element.
    return g_elementClass.make(stateFor(el), [](void*) {
        // Empty for the same reason as makeNodeHandleObject's.
    });
}

HostNodeState* hostNodeStateFor(dom::Node* node) { return stateFor(node); }

HostNodeState* hostNodeStateOfValue(Value v) { return nodeStateOf(v); }

Value makePlainElementValue(dom::Element* el) {
    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);
    return b.get();
}

// The only per-instance properties an element has: its identity. Everything
// else is the same for every element and lives on the prototype.
void decorateElementProto(ObjectBuilder& b);

void installElementGlobals() {
    // `new HTMLElement()` is illegal on the web (the [[HTMLConstructor]] rule),
    // so the body refuses — but every element is born on this prototype, so
    // `el instanceof HTMLElement` answers true, which is the form real library
    // code tests. `Element` is registered as the same object: they are distinct
    // constructors on the web, with HTMLElement extending Element, and one
    // object answering both is closer than two names that brand nothing.
    g_elementClass.install("HTMLElement", 0, nullptr, decorateElementProto);
    g_elementClass.alias("Element");
}

void installElementCore(ObjectBuilder& b, dom::Element* el) {
    b.set("nodeType", ev::fromDouble(1));
    b.set("tagName", ev::fromUtf8(el->tagName()));
    b.set("nodeName", ev::fromUtf8(el->tagName()));

    // The event-target trio stays per instance, and is the only part of the
    // element surface that does. installElementEventTarget takes an
    // ElementSource — a std::function<dom::Element*()> with no receiver to
    // read — and its error messages name the tag, which is per element too.
    // Three methods an element owns instead of fifty-eight.
    installElementEventTarget(b, [st = stateFor(el)]() { return st->el; },
                              el->tagName().c_str());
}

void decorateElementProto(ObjectBuilder& b) {

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

    // ---- style / classList, both built on first read ----------------------
    b.accessor("style",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasStyle && st->el) {
                       Value s = makeStyleObject(st);
                       st->styleObj.set(s);
                       st->hasStyle = true;
                   }
                   return st->styleObj.get();
               },
               nullptr);
    b.accessor("classList",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasClassList && st->el) {
                       Value c = makeClassListObject(st);
                       st->classListObj.set(c);
                       st->hasClassList = true;
                   }
                   return st->classListObj.get();
               },
               nullptr);
    b.accessor("dataset",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasDataset && st->el) {
                       Value d = makeDatasetObject(st);
                       st->datasetObj.set(d);
                       st->hasDataset = true;
                   }
                   return st->datasetObj.get();
               },
               nullptr);

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
    // This is what makes it a DOM rather than a list of styled boxes, and it is
    // why the registry exists: appendChild has to recover a Node* from the
    // value the program is holding, and `children` has to hand the same value
    // back.
    //
    // The node half — parentNode, childNodes, the mutators, cloneNode — is
    // installNodeTree (host_node.cpp), shared with text and comment wrappers.
    // What is left here is the ELEMENT-only half: the views that skip
    // non-elements. `childNodes` and `firstChild` answer text nodes now;
    // `children` and `firstElementChild` are the ones that do not, which is the
    // distinction the web draws and the one an app relies on when it walks a
    // tree it did not build itself.
    installNodeTree(b);

    b.def("append", 1, [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
        if (!st->el) return ev::undefined();
        for (const Value& v : a) {
            // A string argument becomes a text node, as the web's append does.
            // This is the shortest path from compiled code to text in the
            // document, and without it `append("hi")` would silently do
            // nothing.
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
                   if (!st) return ev::undefined();
        if (!st->el) return ev::undefined();
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
    // Straight to the engine's tracking, as js_element_setPointerCapture does.
    // A drag handle that does not capture loses the pointer the moment it
    // leaves the element, which is every slider in every inspector.
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
                   if (!st) return ev::undefined();
        if (st->el) {
            if (auto* e = hostEngine()) {
                e->requestPointerLock(st->el);
            }
        }
        return ev::undefined();
    });

    // ---- fullscreen -------------------------------------------------------
    b.def("requestFullscreen", 0, [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
        if (st->el) {
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

    // ---- focus ------------------------------------------------------------
    b.def("focus", 0, [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
        if (st->el)
            if (dom::Document* doc = st->el->document()) doc->setActiveElement(st->el);
        return ev::undefined();
    });
    b.def("blur", 0, [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
        if (st->el)
            if (dom::Document* doc = st->el->document())
                if (doc->activeElement() == st->el) doc->setActiveElement(nullptr);
        return ev::undefined();
    });

    // ---- form controls ----------------------------------------------------
    // The three.js editor's entire widget library is these properties: every
    // row of its sidebar is an <input>, a <select> or a <textarea> read and
    // written through them, and without them the UI builds and then does
    // nothing. The non-trivial ones — where a <select>'s selection actually
    // lives, what a <textarea>'s value is before anyone has typed in it —
    // route to layout::formValue and friends, the same functions bro's own
    // bindings answer from.
    b.accessor("value",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromUtf8(st->el ? layout::formValue(st->el)
                                              : std::string());
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (st->el && !ev::isObject(v))
                       layout::setFormValue(st->el,
                                            ev::isUndefined(v) ? "" : ev::toUtf8(v));
                   return ev::undefined();
               });
    b.accessor("selectedIndex",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? layout::selectedIndex(st->el) : -1);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (st->el) layout::setSelectedIndex(st->el, i32At(a, 0));
                   return ev::undefined();
               });
    b.accessor("options",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
                   std::vector<dom::Element*> opts = layout::selectOptions(st->el);
                   return hostArrayOf(opts.size(), [&opts](size_t i) {
                       return hostElementValue(opts[i]);
                   });
               },
               nullptr);
    // `checked` writes go through js::clearRadioGroup rather than straight to
    // the attribute: a radio's group is cleared however its checkedness became
    // true, not only by a click, and leaving the old member checked would show
    // two picked radios in a group that can only mean one.
    b.accessor("checked",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromBool(st->el && st->el->hasAttribute("checked"));
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::undefined();
                   if (ev::toBool(argAt(a, 0))) {
                       js::clearRadioGroup(st->el);
                       st->el->setAttribute("checked", "");
                   } else {
                       st->el->removeAttribute("checked");
                   }
                   return ev::undefined();
               });

    // Boolean HTML attributes: PRESENCE means true, whatever the value, so
    // `<input disabled>` (which parses to disabled="") counts. Writing false
    // removes the attribute rather than setting it to "false", which would
    // read back as true.
    auto defBoolAttr = [&b](const char* name, const char* attr) {
        std::string a(attr);
        b.accessor(name,
                   [a](Value self_, std::span<const Value>) {
                       HostNodeState* st = nodeStateOf(self_);
                       return ev::fromBool(st && st->el && st->el->hasAttribute(a));
                   },
                   [a](Value self_, std::span<const Value> args) {
                       HostNodeState* st = nodeStateOf(self_);
                       if (!st || !st->el) return ev::undefined();
                       if (ev::toBool(argAt(args, 0))) st->el->setAttribute(a, "");
                       else st->el->removeAttribute(a);
                       return ev::undefined();
                   });
    };
    defBoolAttr("disabled", "disabled");
    defBoolAttr("selected", "selected");
    defBoolAttr("multiple", "multiple");
    defBoolAttr("required", "required");
    defBoolAttr("readOnly", "readonly");
    defBoolAttr("hidden", "hidden");
    defBoolAttr("autofocus", "autofocus");
    defBoolAttr("draggable", "draggable");

    // Plain string reflections. `type` is the one with a default — an <input>
    // with no type attribute is a text input, and UI code branches on it.
    auto defStrAttr = [&b](const char* name, const char* attr,
                               const char* fallback) {
        std::string a(attr), f(fallback);
        b.accessor(name,
                   [a, f](Value self_, std::span<const Value>) {
                       HostNodeState* st = nodeStateOf(self_);
                       if (!st || !st->el) return ev::fromUtf8("");
                       const std::string& v = st->el->getAttribute(a);
                       return ev::fromUtf8(v.empty() ? f : v);
                   },
                   [a](Value self_, std::span<const Value> args) {
                       HostNodeState* st = nodeStateOf(self_);
                       Value v = argAt(args, 0);
                       if (st && st->el && !ev::isObject(v))
                           st->el->setAttribute(a, ev::isUndefined(v) ? ""
                                                                      : ev::toUtf8(v));
                       return ev::undefined();
                   });
    };
    defStrAttr("type", "type", "text");
    defStrAttr("name", "name", "");
    defStrAttr("placeholder", "placeholder", "");
    defStrAttr("title", "title", "");
    defStrAttr("min", "min", "");
    defStrAttr("max", "max", "");
    defStrAttr("step", "step", "");
    defStrAttr("href", "href", "");
    defStrAttr("download", "download", "");
    defStrAttr("target", "target", "");
    defStrAttr("rel", "rel", "");
    defStrAttr("src", "src", "");
    defStrAttr("alt", "alt", "");
    defStrAttr("accept", "accept", "");
    defStrAttr("autocomplete", "autocomplete", "");

    // tabIndex is a number whose default depends on the tag, so it goes through
    // layout::tabIndex rather than reading the attribute here — see
    // form_control.h for why a flat default is wrong in both directions.
    b.accessor("tabIndex",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? layout::tabIndex(st->el) : -1);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = nodeStateOf(self_);
                   if (!st) return ev::undefined();
                   if (st->el) layout::setTabIndex(st->el, i32At(a, 0));
                   return ev::undefined();
               });

}

}  // namespace bro::bronze_host

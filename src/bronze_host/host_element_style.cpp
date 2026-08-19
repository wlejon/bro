// CSS style proxy, computedStyle, and CSS custom property handling.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/element.h"
#include "dom/style_proxy.h"
#include "engine/engine.h"
#include "layout/computed_style.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

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

}  // namespace

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

void decorateElementStyle(ObjectBuilder& b) {
    // ---- style / classList, both built on first read ----------------------
    b.accessor("style",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->hasStyle && st->el) {
                       Value s = makeStyleObject(st);
                       st->styleObj.set(s);
                       st->hasStyle = true;
                   }
                   return st->styleObj.get();
               },
               nullptr);
}

}  // namespace bro::bronze_host

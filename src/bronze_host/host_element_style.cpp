// CSS style proxy, computedStyle, and CSS custom property handling.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_proxy.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_list_iteration.h"

#include "dom/element.h"
#include "dom/style_proxy.h"
#include "engine/engine.h"
#include "layout/computed_style.h"
#include "css/properties.h"
#include "util/asset_path.h"
#include "util/log.h"
#include "util/remote_asset.h"
#include "bronze_host/host_events.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Whether `css` (a dashed name, as styleKeyToCss gives it) is a property the
// engine supports: every such name is a member of every CSSStyleDeclaration on
// the web, set or not, which is what feature detection asks (`'transform' in
// el.style`, Leaflet's and jQuery's vendor-prefix probes: a `false` there made
// Leaflet write every position to style['false']). `webkitX` / `WebkitX` are
// the -webkit- aliases Blink keeps; Moz, ms and O prefixes are not members.
bool supportedCssProperty(std::string css) {
    static const std::unordered_set<std::string> known = [] {
        std::unordered_set<std::string> s;
        for (const auto& def : htmlayout::css::knownProperties()) s.insert(def.name);
        return s;
    }();
    if (css.size() > 2 && css[0] == '-' && css[1] == '-') return false;  // custom: only when set
    if (css == "css-float") css = "float";
    if (css.rfind("webkit-", 0) == 0) css = "-" + css;
    if (css.rfind("-webkit-", 0) == 0 && !known.count(css)) css.erase(0, 8);
    return known.count(css) > 0 || htmlayout::css::isShorthandProperty(css);
}

}  // namespace

// CSSStyleDeclaration.prototype (js/cssom.js), which element.style and the
// computed style are born on: `instanceof CSSStyleDeclaration`, its toString
// tag and constructor. Their members are the objects' own, answered first.
static Value styleDeclarationProto() {
    ev::GlobalValue hooks = ev::globalValue("__bro_cssom");
    if (!hooks.found || !ev::isObject(hooks.value)) return ev::undefined();
    Value proto = ev::getProperty(hooks.value, "declarationProto");
    return ev::isObject(proto) ? proto : ev::undefined();
}

// What the cssom.js prototype's own accessors would read from the instance:
// answered here so they never reach it.
static void declarationExtras(ObjectBuilder& b, std::function<std::vector<std::string>()> names) {
    b.accessor("parentRule", [](Value, std::span<const Value>) { return ev::null(); }, nullptr);
    installSnapshotIteration(b.get(), [names = std::move(names)]() {
        std::vector<std::string> list = names();
        return hostArrayOf(list.size(), [&list](size_t i) { return ev::fromUtf8(list[i]); });
    });
}

Value makeStyleObject(HostNodeState* raw) {
    StateRef st(raw);
    ObjectBuilder b;
    b.def("setProperty", 2, [st](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0), valV = argAt(a, 1);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::undefined();
        std::string name = ev::toUtf8(nameV);
        std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV))
                              ? ev::toUtf8(valV) : "";
        bool allWhite = true;
        for (char ch : val) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                allWhite = false;
                break;
            }
        }
        if (allWhite) st->el->style().removeProperty(name);
        else st->el->style().setProperty(name, val);
        return ev::undefined();
    });
    b.def("getPropertyValue", 1, [st](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (!st->el || ev::isObject(nameV) || ev::isUndefined(nameV))
            return ev::fromUtf8("");
        return ev::fromUtf8(st->el->style().getProperty(ev::toUtf8(nameV)));
    });
    b.def("getPropertyPriority", 1, [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
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
    b.def("item", 1, [st](Value, std::span<const Value> a) {
        if (!st->el || a.empty()) return ev::fromUtf8("");
        size_t idx = satCast<size_t>(ev::toDouble(a[0]));
        const auto& props = st->el->style().properties();
        if (idx >= props.size()) return ev::fromUtf8("");
        auto it = props.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });
    b.accessor("length",
               [st](Value, std::span<const Value>) {
                   if (!st->el) return ev::fromDouble(0.0);
                   return ev::fromDouble(static_cast<double>(st->el->style().size()));
               },
               nullptr);
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

    declarationExtras(b, [st]() {
        std::vector<std::string> names;
        if (st->el)
            for (const auto& [name, value] : st->el->style().properties()) names.push_back(name);
        return names;
    });

    HostProxyTraps t;
    t.methods = b.get();
    t.proto = styleDeclarationProto();
    // Every CSS property is a property of this object, set or not — that is
    // what makes `style.gridAutoFlow` readable without a table saying so, and
    // it is also what the web does: an unset property reads "".
    t.get = [st](const std::string& key, Value& out) {
        if (!st->el) return false;
        bool isIndex = !key.empty();
        for (char c : key) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                isIndex = false;
                break;
            }
        }
        if (isIndex) {
            size_t idx = static_cast<size_t>(std::stoul(key));
            const auto& props = st->el->style().properties();
            if (idx < props.size()) {
                auto it = props.begin();
                std::advance(it, idx);
                out = ev::fromUtf8(it->first);
                return true;
            }
            out = ev::undefined();
            return true;
        }
        out = ev::fromUtf8(st->el->style().getProperty(styleKeyToCss(key)));
        return true;
    };
    t.set = [st](const std::string& key, Value v) {
        if (!st->el || ev::isObject(v)) return;
        if (key == "cssText") {
            st->el->style().setCssText(ev::isUndefined(v) || ev::isNull(v) ? "" : ev::toUtf8(v));
            return;
        }
        const std::string css = styleKeyToCss(key);
        if (ev::isUndefined(v) || ev::isNull(v)) {
            st->el->style().removeProperty(css);
        } else {
            std::string val = ev::toUtf8(v);
            bool allWhite = true;
            for (char ch : val) {
                if (!std::isspace(static_cast<unsigned char>(ch))) {
                    allWhite = false;
                    break;
                }
            }
            if (allWhite) st->el->style().removeProperty(css);
            else st->el->style().setProperty(css, val);
        }
    };
    // Membership answers for every supported property, set or not, as on the
    // web (supportedCssProperty), plus a set custom property. Enumeration
    // answers for the SET properties only, which keeps `Object.keys(el.style)`
    // the short useful list rather than all 363.
    t.has = [st](const std::string& key) {
        if (!st->el) return false;
        const std::string css = styleKeyToCss(key);
        return supportedCssProperty(css) || !st->el->style().getProperty(css).empty();
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
// A computed url() is an absolute URL, quoted, as a browser serialises it:
// url("bro://app/css/img/a.png") for `url(img/a.png)` in css/s.css, which
// the cascade has already rebased onto the sheet's file. Leaflet finds its
// marker images by reading this off a probe element.
static std::string absoluteCssUrls(const std::string& value, dom::Document* doc) {
    if (value.find("url(") == std::string::npos) return value;
    std::string out;
    size_t i = 0;
    while (i < value.size()) {
        size_t at = value.find("url(", i);
        if (at == std::string::npos) { out.append(value, i, std::string::npos); break; }
        size_t close = value.find(')', at + 4);
        if (close == std::string::npos) { out.append(value, i, std::string::npos); break; }
        out.append(value, i, at - i);
        std::string body = value.substr(at + 4, close - at - 4);
        if (body.size() >= 2 && (body.front() == '"' || body.front() == '\'') && body.back() == body.front())
            body = body.substr(1, body.size() - 2);
        std::string unescaped;
        for (size_t k = 0; k < body.size(); ++k) {
            if (body[k] == '\\' && k + 1 < body.size()) ++k;
            unescaped += body[k];
        }
        std::string url;
        if (unescaped.rfind("data:", 0) == 0) {
            url = unescaped;
        } else if ((!unescaped.empty() && unescaped[0] == '/' && unescaped.rfind("//", 0) != 0) ||
                   util::isDrivePath(unescaped)) {
            url = hostEngine() ? hostEngine()->appUrlForPath(unescaped) : std::string();
            if (url.empty()) {
                std::string path = unescaped;
                std::replace(path.begin(), path.end(), '\\', '/');
                url = (util::isDrivePath(path) ? "file:///" : "file://") + path;
            }
        } else {
            url = util::resolveUrl(documentBaseUrl(doc), unescaped);
        }
        out += "url(\"";
        for (char ch : url) { if (ch == '"' || ch == '\\') out += '\\'; out += ch; }
        out += "\")";
        i = close + 1;
    }
    return out;
}

Value makeComputedStyleObject(HostNodeState* raw) {
    StateRef st(raw);
    ObjectBuilder b;
    auto resolve = [st](const std::string& css) {
        if (!st->el) return std::string();
        hostEngine()->flushLayoutForRead(st->el->document());
        return absoluteCssUrls(layout::computedProperty(st->el, css, hostEngine()->textMetrics()),
                               st->el->document());
    };
    b.def("getPropertyValue", 1, [resolve](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        if (ev::isObject(nameV) || ev::isUndefined(nameV)) return ev::fromUtf8("");
        return ev::fromUtf8(resolve(ev::toUtf8(nameV)));
    });
    b.def("getPropertyPriority", 1, [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
    });
    b.def("item", 1, [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
    });
    b.accessor("length", [](Value, std::span<const Value>) {
        return ev::fromDouble(0.0);
    }, nullptr);
    b.accessor("cssText", [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
    }, nullptr);
    // Present and inert, because that is what they are on the web's computed
    // declaration — a UI that calls them on the wrong object gets the web's
    // behaviour rather than a TypeError from a missing method.
    b.def("setProperty", 2, [](Value, std::span<const Value>) {
        return ev::undefined();
    });
    b.def("removeProperty", 1, [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
    });

    declarationExtras(b, []() { return std::vector<std::string>(); });

    HostProxyTraps t;
    t.methods = b.get();
    t.proto = styleDeclarationProto();
    t.get = [resolve](const std::string& key, Value& out) {
        out = ev::fromUtf8(resolve(styleKeyToCss(key)));
        return true;
    };
    // No `set` and no `remove`: the web's computed declaration ignores both,
    // and leaving the callbacks empty is how this file spells that.
    t.has = [resolve](const std::string& key) {
        const std::string css = styleKeyToCss(key);
        return supportedCssProperty(css) || !resolve(css).empty();
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
    hostSweepTouch(st);
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
        const Rooted el(elValue);
        Value c = makeComputedStyleObject(st);
        // makeComputedStyleObject allocates, so the entry pointer is re-taken
        // from the element's CURRENT address rather than trusted across the call.
        st = static_cast<HostNodeState*>(ev::handleData(el));
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

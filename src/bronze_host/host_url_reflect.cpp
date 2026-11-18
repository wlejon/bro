// URL-valued reflected attributes and HTMLHyperlinkElementUtils.
//
// `a.href`, `area.href`, `link.href`, `img.src`, a media element's `src` read
// as the attribute resolved against the element's document's base URL (HTML
// §2.6.1 "reflect" for USVString URL attributes; `base.href` against the
// document's own URL), "" without the attribute, the attribute itself when
// it does not resolve. An <a> or <area> also has the URL's parts —
// `protocol`, `username`, `password`, `host`, `hostname`, `port`,
// `pathname`, `search`, `hash`, `origin` — readable and writable (a write
// rewrites href), the parts parsed by the realm's URL class.

#include "bronze_host/host_builder.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_url_reflect.h"
#include "bronze_host/host_values.h"

#include "dom/document.h"
#include "dom/element.h"
#include "util/remote_asset.h"

#include <cctype>
#include <span>
#include <string>

namespace bro::bronze_host {

namespace {

std::string lowerTag(const dom::Element* el) {
    std::string t = el->tagName();
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

// A URL object for `href` (the realm's URL class), or undefined when it does
// not parse.
Value urlObject(const std::string& href) {
    ev::GlobalValue ctor = ev::globalValue("URL");
    if (!ctor.found || !ev::isFunction(ctor.value)) return ev::undefined();
    const ev::Persistent ctorP(ctor.value);
    const ev::Persistent arg(ev::fromUtf8(href));
    Value a = arg.get();
    ev::CallResult r = ev::construct(ctorP.get(), std::span<const Value>(&a, 1));
    return r.thrown ? ev::undefined() : r.value;
}

constexpr const char* kParts[] = {"protocol", "username", "password", "host", "hostname",
                                  "port", "pathname", "search", "hash", "origin"};

}  // namespace

std::string reflectedUrl(const dom::Element* el, const char* attr) {
    if (!el) return {};
    dom::Document* doc = el->document();
    // A <base> resolves against the document's own URL, and without an href
    // is that URL.
    if (lowerTag(el) == "base") {
        return el->hasAttribute(attr) ? util::resolveUrl(documentUrl(doc), el->getAttribute(attr)) : documentUrl(doc);
    }
    if (!el->hasAttribute(attr)) return {};
    return util::resolveUrl(documentBaseUrl(doc), el->getAttribute(attr));
}

void decorateRelList(ObjectBuilder& b) {
    b.accessor(
        "relList",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self);
            return st && st->el ? makeTokenListObject(st, "rel") : ev::undefined();
        },
        nullptr);
}

void decorateHyperlinkProto(ObjectBuilder& b) {
    for (const char* part : kParts) {
        const std::string name = part;
        b.accessor(
            part,
            [name](Value self, std::span<const Value>) -> Value {
                HostNodeState* st = hostNodeStateOfValue(self);
                if (!st || !st->el) return ev::undefined();
                const std::string href = reflectedUrl(st->el, "href");
                if (href.empty()) return ev::fromUtf8(name == "origin" ? "null" : "");
                const ev::Persistent url(urlObject(href));
                if (!ev::isObject(url.get())) return ev::fromUtf8(name == "origin" ? "null" : "");
                return ev::getProperty(url.get(), name.c_str());
            },
            [name](Value self, std::span<const Value> a) -> Value {
                const ev::Persistent receiver(self);
                const ev::Persistent value(argAt(a, 0));
                HostNodeState* st = hostNodeStateOfValue(receiver.get());
                if (!st || !st->el || name == "origin") return ev::undefined();
                const ev::Persistent url(urlObject(reflectedUrl(st->el, "href")));
                if (!ev::isObject(url.get())) return ev::undefined();
                // Through Reflect.set, so URL's own setter runs (a host
                // setProperty would define a data property instead).
                ev::GlobalValue reflect = ev::globalValue("Reflect");
                if (!reflect.found) return ev::undefined();
                const ev::Persistent reflectP(reflect.value);
                const ev::Persistent set(ev::getProperty(reflectP.get(), "set"));
                const ev::Persistent key(ev::fromUtf8(name));
                Value args[3] = {url.get(), key.get(), value.get()};
                ev::call(set.get(), reflectP.get(), std::span<const Value>(args, 3));
                const std::string href = ev::toUtf8(ev::getProperty(url.get(), "href"));
                if (HostNodeState* again = hostNodeStateOfValue(receiver.get()); again && again->el) {
                    again->el->setAttribute("href", href);
                }
                return ev::undefined();
            });
    }
    decorateRelList(b);
    b.def("toString", 0, [](Value self, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self);
        return ev::fromUtf8(st && st->el ? reflectedUrl(st->el, "href") : std::string());
    });
}

}  // namespace bro::bronze_host

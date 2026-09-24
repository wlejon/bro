// The `CSS` namespace object: CSS.supports() and CSS.escape().
//
// supports() runs the same probes @supports does (htmlayout's parser), so a
// feature test in script and one in a stylesheet agree. escape() is the
// CSSOM "serialize an identifier" algorithm, for building selectors out of
// arbitrary ids and class names.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "css/parser.h"

#include <cstdio>
#include <string>

namespace bro::bronze_host {

namespace {

// CSSOM §2.1 serialize an identifier, over UTF-8. Code points at or above
// U+0080 pass through untouched, so multi-byte sequences are copied whole.
std::string cssEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    const size_t n = in.size();
    auto hexEscape = [&out](unsigned c) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "\\%x ", c);
        out += buf;
    };
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0) { out += "\xEF\xBF\xBD"; continue; }  // U+FFFD
        if ((c >= 0x01 && c <= 0x1F) || c == 0x7F) { hexEscape(c); continue; }
        const bool digit = c >= '0' && c <= '9';
        if (i == 0 && digit) { hexEscape(c); continue; }
        if (i == 1 && digit && in[0] == '-') { hexEscape(c); continue; }
        if (i == 0 && c == '-' && n == 1) { out += "\\-"; continue; }
        if (c >= 0x80 || c == '-' || c == '_' || digit ||
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            out += static_cast<char>(c);
            continue;
        }
        out += '\\';
        out += static_cast<char>(c);
    }
    return out;
}

}  // namespace

void installCssNamespace() {
    ObjectBuilder b;
    // CSS.supports(property, value) or CSS.supports(conditionText).
    b.def("supports", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("CSS.supports: 1 argument required");
        if (a.size() >= 2) {
            return ev::fromBool(htmlayout::css::supportsDeclaration(ev::toUtf8(a[0]),
                                                                    ev::toUtf8(a[1])));
        }
        return ev::fromBool(htmlayout::css::supportsCondition(ev::toUtf8(a[0])));
    });
    b.def("escape", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("CSS.escape: 1 argument required");
        return ev::fromUtf8(cssEscape(ev::toUtf8(a[0])));
    });
    ev::registerGlobal("CSS", b.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::Persistent global(gt.value);
        ev::Persistent v(ev::globalValue("CSS").value);
        ev::setProperty(global.get(), "CSS", v.get());
    }
}

}  // namespace bro::bronze_host

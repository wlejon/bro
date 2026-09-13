// URL and URLSearchParams — Web-standard URL resolution and query parameters.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "util/log.h"
#include "util/object_url.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// URL
// ---------------------------------------------------------------------------

Value makeCreateObjectURL() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            const HostBlob* blob = hostBlobOf(argAt(a, 0));
            if (!blob)
                return ev::throwTypeError(
                    "URL.createObjectURL: argument is not a Blob");
            // Minted here rather than by the engine, and numbered from a
            // counter of its own, because bro's JS half mints from its own
            // counter into the SAME table — two counters, one namespace, so
            // the prefix has to differ or the two would collide.
            static std::atomic<uint64_t> counter{1};
            const std::string url =
                "blob:bro/bronze-" +
                std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
            util::registerObjectURL(url, blob->bytes, blob->type);
            return ev::fromUtf8(url);
        },
        1);
}

Value makeRevokeObjectURL() {
    return ev::makeFunction(
        [](Value, std::span<const Value> a) {
            Value v = argAt(a, 0);
            if (!ev::isObject(v) && !ev::isUndefined(v))
                util::revokeObjectURL(ev::toUtf8(v));
            return ev::undefined();
        },
        1);
}

// The URL parser: enough of RFC 3986 to answer the components libraries read.
// Not a validator — an input with no scheme resolves against `base` when one is
// given and is reported as-is when it is not, which is where three.js's
// LoaderUtils and every "is this absolute" test land.
struct ParsedURL {
    std::string href, protocol, username, password, hostname, port, pathname, search, hash;
};

inline constexpr uint32_t kHostUrlTag = 0x55524C20u;  // 'URL '

// The parse is SHARED, not owned. `url.searchParams` hands out an object whose
// methods read and write this state, and that object can outlive the URL it
// came from (`const p = new URL(s).searchParams`) — with a raw back-pointer
// that is a use-after-free the moment the finalizer runs. A shared_ptr costs
// one allocation per URL and makes the detached case merely useless rather
// than fatal. It cannot be an ev::Persistent on the URL object for the reason
// at the top of host_internal.h: hostUrlDtor is a handle finalizer.
struct HostUrl {
    uint32_t tag = kHostUrlTag;
    std::shared_ptr<ParsedURL> parsed = std::make_shared<ParsedURL>();
};

void hostUrlDtor(void* p) { delete static_cast<HostUrl*>(p); }

HostUrl* urlOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* u = static_cast<HostUrl*>(ev::handleData(v));
    if (!u || u->tag != kHostUrlTag) return nullptr;
    return u;
}

bool splitAbsolute(const std::string& in, ParsedURL& out) {
    const size_t colon = in.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        const char c = in[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!ok) return false;
    }
    out.protocol = in.substr(0, colon + 1);
    std::string rest = in.substr(colon + 1);

    if (rest.rfind("//", 0) == 0) {
        rest = rest.substr(2);
        const size_t cut = rest.find_first_of("/?#");
        std::string authority = cut == std::string::npos ? rest : rest.substr(0, cut);
        rest = cut == std::string::npos ? std::string() : rest.substr(cut);
        const size_t at = authority.rfind('@');
        if (at != std::string::npos) {
            std::string userinfo = authority.substr(0, at);
            authority = authority.substr(at + 1);
            size_t colon = userinfo.find(':');
            if (colon != std::string::npos) {
                out.username = userinfo.substr(0, colon);
                out.password = userinfo.substr(colon + 1);
            } else {
                out.username = userinfo;
            }
        }
        const size_t portColon = authority.rfind(':');
        if (portColon != std::string::npos &&
            authority.find_first_not_of("0123456789", portColon + 1) ==
                std::string::npos) {
            out.port = authority.substr(portColon + 1);
            authority = authority.substr(0, portColon);
        }
        out.hostname = authority;
    }

    const size_t hash = rest.find('#');
    if (hash != std::string::npos) {
        out.hash = rest.substr(hash);
        rest = rest.substr(0, hash);
    }
    const size_t query = rest.find('?');
    if (query != std::string::npos) {
        out.search = rest.substr(query);
        rest = rest.substr(0, query);
    }
    out.pathname = rest.empty() && !out.hostname.empty() ? "/" : rest;
    return true;
}

// The relative-reference merge, in the one form apps actually use: an absolute
// path replaces the base's path, anything else is appended to the base's
// directory. `..` and `.` segments are then removed, because a resolved URL
// with them in it is not equal to the one every other implementation produces.
std::string normalizePath(const std::string& path) {
    std::vector<std::string> parts;
    size_t i = 0;
    const bool absolute = !path.empty() && path[0] == '/';
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        const std::string seg = path.substr(i, slash - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        i = slash + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        out += parts[k];
        if (k + 1 < parts.size()) out += '/';
    }
    // A trailing slash in the input survives normalisation, which matters for
    // a base URL naming a directory.
    if (!path.empty() && path.back() == '/' && !out.empty() && out.back() != '/')
        out += '/';
    return out;
}

void rebuildUrlHref(ParsedURL& u) {
    u.href = u.protocol;
    if (!u.hostname.empty() || !u.username.empty()) {
        u.href += "//";
        if (!u.username.empty() || !u.password.empty()) {
            u.href += u.username;
            if (!u.password.empty()) {
                u.href += ":" + u.password;
            }
            u.href += "@";
        }
        u.href += u.hostname;
        if (!u.port.empty()) u.href += ":" + u.port;
    }
    u.href += u.pathname + u.search + u.hash;
}

bool parseURL(const std::string& input, const std::string& base, ParsedURL& out) {
    if (splitAbsolute(input, out)) {
        out.pathname = normalizePath(out.pathname);
    } else if (!base.empty()) {
        ParsedURL b;
        if (!splitAbsolute(base, b)) {
            return false;
        }
        out.protocol = b.protocol;
        out.username = b.username;
        out.password = b.password;
        out.hostname = b.hostname;
        out.port = b.port;
        std::string rest = input;
        const size_t hash = rest.find('#');
        if (hash != std::string::npos) {
            out.hash = rest.substr(hash);
            rest = rest.substr(0, hash);
        }
        const size_t query = rest.find('?');
        if (query != std::string::npos) {
            out.search = rest.substr(query);
            rest = rest.substr(0, query);
        }
        if (!rest.empty() && rest[0] == '/') {
            out.pathname = normalizePath(rest);
        } else {
            const size_t slash = b.pathname.rfind('/');
            const std::string dir = slash == std::string::npos
                                        ? "/"
                                        : b.pathname.substr(0, slash + 1);
            out.pathname = normalizePath(dir + rest);
        }
    } else {
        return false;
    }

    rebuildUrlHref(out);
    return true;
}

// The query string is application/x-www-form-urlencoded, and the two halves of
// that are NOT optional decoration: a value carrying `&` or `=` re-parses as
// extra pairs if it is written raw, so a program that puts a user string into a
// query would silently build a different URL than it asked for. Reading is the
// mirror — `?q=a%20b` is the byte string "a b", and the caller wants the bytes.
// Byte-oriented on purpose: the payload is UTF-8 and each byte encodes on its
// own, which is what the URL standard's percent-encoder does.

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string formUrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            const int hi = hexNibble(in[i + 1]);
            const int lo = hexNibble(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                // Not a valid escape. The URL standard keeps the bytes rather
                // than failing, so `%zz` round-trips as itself.
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

std::string formUrlEncode(const std::string& in) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

// Decoded pairs. `search` is the raw `?...` slice off the URL, so everything
// coming out of here is the byte string the program means, not the wire form.
std::vector<std::pair<std::string, std::string>> parseQueryParams(const std::string& search) {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (search.size() > 1 && search[0] == '?') {
        std::string q = search.substr(1);
        size_t i = 0;
        while (i <= q.size()) {
            size_t amp = q.find('&', i);
            if (amp == std::string::npos) amp = q.size();
            const std::string item = q.substr(i, amp - i);
            if (!item.empty()) {
                const size_t eq = item.find('=');
                if (eq == std::string::npos)
                    pairs.emplace_back(formUrlDecode(item), "");
                else
                    pairs.emplace_back(formUrlDecode(item.substr(0, eq)),
                                       formUrlDecode(item.substr(eq + 1)));
            }
            i = amp + 1;
        }
    }
    return pairs;
}

// The `=` is written even for an empty value: the standard's serializer always
// emits it, so `?a=` round-trips instead of decaying to `?a`.
std::string serializeQueryParams(const std::vector<std::pair<std::string, std::string>>& pairs) {
    if (pairs.empty()) return "";
    std::string out;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) out += '&';
        out += formUrlEncode(pairs[i].first);
        out += '=';
        out += formUrlEncode(pairs[i].second);
    }
    return out;
}

inline constexpr uint32_t kHostSearchParamsTag = 0x53504152u;  // 'SPAR'

struct HostSearchParams {
    uint32_t tag = kHostSearchParamsTag;
    std::shared_ptr<ParsedURL> parsed;
};

void hostSearchParamsDtor(void* p) { delete static_cast<HostSearchParams*>(p); }

HostSearchParams* searchParamsOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* sp = static_cast<HostSearchParams*>(ev::handleData(v));
    if (!sp || sp->tag != kHostSearchParamsTag) return nullptr;
    return sp;
}

HostClass g_urlSearchParamsClass;

Value makeSearchParamsValue(const std::shared_ptr<ParsedURL>& parsed) {
    auto* sp = new HostSearchParams();
    sp->parsed = parsed;
    return g_urlSearchParamsClass.make(sp, hostSearchParamsDtor);
}

void decorateSearchParamsProto(ObjectBuilder& b) {
    b.def("get", 1, [](Value self, std::span<const Value> a) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed) return ev::null();
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(sp->parsed->search);
        for (const auto& kv : pairs)
            if (kv.first == key) return ev::fromUtf8(kv.second);
        return ev::null();
    });
    b.def("has", 1, [](Value self, std::span<const Value> a) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed) return ev::fromBool(false);
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(sp->parsed->search);
        for (const auto& kv : pairs)
            if (kv.first == key) return ev::fromBool(true);
        return ev::fromBool(false);
    });
    b.def("getAll", 1, [](Value self, std::span<const Value> a) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed)
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(sp->parsed->search);
        std::vector<std::string> hits;
        for (const auto& kv : pairs)
            if (kv.first == key) hits.push_back(kv.second);
        return hostArrayOf(hits.size(),
                           [&hits](size_t i) { return ev::fromUtf8(hits[i]); });
    });
    b.def("set", 2, [](Value self, std::span<const Value> a) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed) return ev::undefined();
        std::string key = ev::toUtf8(argAt(a, 0));
        std::string val = ev::toUtf8(argAt(a, 1));
        auto pairs = parseQueryParams(sp->parsed->search);
        bool found = false;
        std::vector<std::pair<std::string, std::string>> next;
        for (auto& kv : pairs) {
            if (kv.first == key) {
                if (!found) {
                    next.emplace_back(key, val);
                    found = true;
                }
            } else {
                next.push_back(kv);
            }
        }
        if (!found) next.emplace_back(key, val);
        std::string q = serializeQueryParams(next);
        sp->parsed->search = q.empty() ? "" : "?" + q;
        rebuildUrlHref(*sp->parsed);
        return ev::undefined();
    });
    b.def("append", 2, [](Value self, std::span<const Value> a) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed) return ev::undefined();
        std::string key = ev::toUtf8(argAt(a, 0));
        std::string val = ev::toUtf8(argAt(a, 1));
        auto pairs = parseQueryParams(sp->parsed->search);
        pairs.emplace_back(key, val);
        std::string q = serializeQueryParams(pairs);
        sp->parsed->search = q.empty() ? "" : "?" + q;
        rebuildUrlHref(*sp->parsed);
        return ev::undefined();
    });
    b.def("delete", 1, [](Value self, std::span<const Value> a) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed) return ev::undefined();
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(sp->parsed->search);
        std::vector<std::pair<std::string, std::string>> next;
        for (const auto& kv : pairs) {
            if (kv.first != key) next.push_back(kv);
        }
        std::string q = serializeQueryParams(next);
        sp->parsed->search = q.empty() ? "" : "?" + q;
        rebuildUrlHref(*sp->parsed);
        return ev::undefined();
    });
    b.def("toString", 0, [](Value self, std::span<const Value>) {
        HostSearchParams* sp = searchParamsOf(self);
        if (!sp || !sp->parsed) return ev::fromUtf8("");
        auto pairs = parseQueryParams(sp->parsed->search);
        return ev::fromUtf8(serializeQueryParams(pairs));
    });
}

HostClass g_urlClass;

Value makeURLValue(const ParsedURL& u) {
    auto* url = new HostUrl();
    *url->parsed = u;
    return g_urlClass.make(url, hostUrlDtor);
}

void decorateUrlProto(ObjectBuilder& b) {
    b.accessor("href",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->href : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (ev::isObject(v) || ev::isUndefined(v)) return ev::undefined();
                   ParsedURL p;
                   if (parseURL(ev::toUtf8(v), "", p)) {
                       *u->parsed = p;
                   }
                   return ev::undefined();
               });
    b.accessor("origin",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   if (!u || u->parsed->hostname.empty()) return ev::fromUtf8("null");
                   std::string orig = u->parsed->protocol + "//" + u->parsed->hostname;
                   if (!u->parsed->port.empty()) orig += ":" + u->parsed->port;
                   return ev::fromUtf8(orig);
               },
               nullptr);
    b.accessor("protocol",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->protocol : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       std::string s = ev::toUtf8(v);
                       if (!s.empty()) {
                           if (s.back() != ':') s += ':';
                           u->parsed->protocol = s;
                           rebuildUrlHref(*u->parsed);
                       }
                   }
                   return ev::undefined();
               });
    b.accessor("username",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->username : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       u->parsed->username = ev::toUtf8(v);
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("password",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->password : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       u->parsed->password = ev::toUtf8(v);
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("host",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::fromUtf8("");
                   return ev::fromUtf8(u->parsed->port.empty()
                                           ? u->parsed->hostname
                                           : u->parsed->hostname + ":" + u->parsed->port);
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       std::string s = ev::toUtf8(v);
                       size_t colon = s.find(':');
                       if (colon != std::string::npos) {
                           u->parsed->hostname = s.substr(0, colon);
                           u->parsed->port = s.substr(colon + 1);
                       } else {
                           u->parsed->hostname = s;
                           u->parsed->port.clear();
                       }
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("hostname",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->hostname : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       u->parsed->hostname = ev::toUtf8(v);
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("port",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->port : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       u->parsed->port = ev::toUtf8(v);
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("pathname",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->pathname : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       std::string p = ev::toUtf8(v);
                       if (p.empty() || p[0] != '/') p = "/" + p;
                       u->parsed->pathname = normalizePath(p);
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("search",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->search : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       std::string s = ev::toUtf8(v);
                       if (!s.empty() && s[0] != '?') s = "?" + s;
                       u->parsed->search = s;
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    b.accessor("hash",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   return ev::fromUtf8(u ? u->parsed->hash : "");
               },
               [](Value self, std::span<const Value> a) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v) && !ev::isUndefined(v)) {
                       std::string h = ev::toUtf8(v);
                       if (!h.empty() && h[0] != '#') h = "#" + h;
                       u->parsed->hash = h;
                       rebuildUrlHref(*u->parsed);
                   }
                   return ev::undefined();
               });
    // ONE object per URL, cached on the URL itself. The web guarantees
    // `u.searchParams === u.searchParams`, and libraries lean on it — a fresh
    // object per read makes every such check fail. It lives as an ordinary
    // property rather than an ev::Persistent in HostUrl for the reason at the
    // top of host_internal.h: hostUrlDtor is a handle finalizer.
    b.accessor("searchParams",
               [](Value self, std::span<const Value>) {
                   HostUrl* u = urlOf(self);
                   if (!u) return ev::undefined();
                   Value cached = ev::getProperty(self, "_searchParams");
                   if (ev::isObject(cached)) return cached;
                   ev::Persistent owner(self);
                   ev::Persistent made(makeSearchParamsValue(u->parsed));
                   ev::setProperty(owner.get(), "_searchParams", made.get());
                   return made.get();
               },
               nullptr);

    b.def("toString", 0, [](Value self, std::span<const Value>) {
        HostUrl* u = urlOf(self);
        return ev::fromUtf8(u ? u->parsed->href : "");
    });
    b.def("toJSON", 0, [](Value self, std::span<const Value>) {
        HostUrl* u = urlOf(self);
        return ev::fromUtf8(u ? u->parsed->href : "");
    });
}


}  // namespace

void installUrlGlobals() {
    // URL IS A CONSTRUCTOR, and was a bare namespace until it could be one.
    // The blocker was embed::setProperty, which called fatal() on any receiver
    // that was not a plain object, so a callable URL had nowhere to hang
    // createObjectURL; the fallback (the program's own Object.assign) was a
    // hard runtime error rather than a catchable throw, leaving no way to probe
    // at startup and degrade. setProperty takes a FUNCTION receiver now, which
    // lands a static where a class `static` member's definition would go, so
    // all three shapes are available at once: `new URL(href, base)`,
    // `URL.createObjectURL`, and `x instanceof URL` off the real slot-backed
    // prototype (host_image.cpp works the same pattern end to end for `Image`).
    // `URL.parse` stays beside the constructor rather than behind it: it is a
    // real 2024 addition to the web platform, and it answers null where the
    // constructor throws.
    g_urlClass.install(
        "URL", 1,
        [](Value, std::span<const Value> a) {
            Value urlV = argAt(a, 0);
            if (ev::isObject(urlV) || ev::isUndefined(urlV)) {
                return ev::throwTypeError("URL constructor: first argument must be a string");
            }
            std::string urlStr = ev::toUtf8(urlV);
            std::string baseStr;
            Value baseV = argAt(a, 1);
            if (!ev::isObject(baseV) && !ev::isUndefined(baseV)) {
                baseStr = ev::toUtf8(baseV);
            }
            ParsedURL p;
            if (!parseURL(urlStr, baseStr, p)) {
                return ev::throwTypeError("Invalid URL: " + urlStr);
            }
            return makeURLValue(p);
        },
        decorateUrlProto);

    g_urlClass.setStatic("createObjectURL", makeCreateObjectURL());
    g_urlClass.setStatic("revokeObjectURL", makeRevokeObjectURL());
    g_urlClass.setStatic("parse", ev::makeFunction([](Value, std::span<const Value> a) {
        Value hrefV = argAt(a, 0);
        if (ev::isObject(hrefV) || ev::isUndefined(hrefV)) return ev::null();
        Value baseV = argAt(a, 1);
        const std::string base =
            (ev::isObject(baseV) || ev::isUndefined(baseV)) ? "" : ev::toUtf8(baseV);
        ParsedURL p;
        if (!parseURL(ev::toUtf8(hrefV), base, p)) return ev::null();
        return makeURLValue(p);
    }, 2));

    g_urlSearchParamsClass.install(
        "URLSearchParams", 1,
        [](Value, std::span<const Value> a) {
            auto parsed = std::make_shared<ParsedURL>();
            if (!a.empty()) {
                Value initV = a[0];
                if (HostSearchParams* other = searchParamsOf(initV)) {
                    if (other->parsed) parsed->search = other->parsed->search;
                } else if (!ev::isUndefined(initV) && !ev::isNull(initV)) {
                    std::string s = ev::toUtf8(initV);
                    if (!s.empty() && s[0] != '?') s = "?" + s;
                    parsed->search = s;
                }
            }
            return makeSearchParamsValue(parsed);
        },
        decorateSearchParamsProto);
}

}  // namespace bro::bronze_host

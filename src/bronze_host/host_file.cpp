// Blob, File, FileReader, and URL — bytes an app holds, and the names it gives
// them.
//
// Everything else in this layer moves data one way: the app asks for a file and
// the host reads it off disk. This is the other direction. A Blob is bytes the
// PROGRAM produced or was handed, with no path behind them, and the whole point
// of the family is that those bytes can then be used everywhere a URL can —
// `img.src = URL.createObjectURL(blob)`, `fetch(objectUrl)`, an XHR. Without it
// a compiled app can decode a model it loaded and can build one in memory, and
// has no way to show the second.
//
// FOUR THINGS ARE WORTH READING BEFORE THE CODE.
//
// THE OBJECT-URL TABLE IS THE ENGINE'S, not this layer's. `util::object_url.h`
// is a process-global map from a `blob:` string to bytes, already consulted by
// the <img> size probe, the draw path's image cache and bro's own JS. Minting
// into it means a URL a compiled app creates resolves in the PAGE's markup and
// in an interpreted script beside it, which is the same argument that put
// listeners on the engine's dispatch instead of a second one (host_dom_events.cpp).
// A private table here would have produced URLs only compiled code could read,
// and the failure would have been an <img> that silently showed nothing.
//
// THE BYTES ARE HOST MEMORY. A HostBlob owns a std::vector, not heap bytes.
// embed::typedArrayInfo's pointer dies at the next allocation (embed.h says so
// loudly); a Blob has to outlive arbitrary program execution, so the
// constructor copies out of the heap immediately and never looks back. It is
// also what lets `URL.createObjectURL` hand the engine a resource that any
// thread can read.
//
// A READ IS ASYNCHRONOUS BECAUSE THE WEB SAYS SO, not because it is slow. Every
// read here is a memcpy and could return before the call does. It goes through
// postHostTask anyway, so `reader.onload` is assigned AFTER `readAsText` is
// called and still fires — which is how every FileReader ever written is
// structured, and a synchronous implementation would run the callback that does
// not exist yet.
//
// `URL` IS A NAMESPACE, NOT A CONSTRUCTOR. On the web it is both — callable and
// carrying createObjectURL — and the host cannot build that shape at all. The
// long comment at installFileGlobals() has the reason and the exact runtime
// message; the short version is that `URL.parse(href, base)` is here and
// `new URL(href)` is not.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "util/log.h"
#include "util/object_url.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

void hostBlobDtor(void* p) { delete static_cast<HostBlob*>(p); }

HostBlob* mutableHostBlob(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* b = static_cast<HostBlob*>(ev::handleData(v));
    if (!b || b->tag != kHostBlobTag) return nullptr;
    return b;
}

// ---------------------------------------------------------------------------
// Assembling a Blob's bytes out of whatever the program passed
// ---------------------------------------------------------------------------

// One part of `new Blob(parts)`. The web accepts strings, ArrayBuffers, any
// view over one, and other Blobs; each is appended in order.
//
// COPIED IMMEDIATELY, before anything else can allocate. typedArrayInfo hands
// back a pointer into the moving heap that the very next embed call may
// invalidate, so the memcpy has to happen between the info call and the loop's
// next iteration — which is exactly where it is.
void appendPart(std::vector<uint8_t>& out, Value part) {
    if (const HostBlob* nested = hostBlobOf(part)) {
        out.insert(out.end(), nested->bytes.begin(), nested->bytes.end());
        return;
    }
    if (ev::isTypedArray(part)) {
        ev::TypedArrayInfo info = ev::typedArrayInfo(part);
        if (info) out.insert(out.end(), info.data, info.data + info.byteLength);
        return;
    }
    if (ev::isArrayBuffer(part)) {
        ev::ArrayBufferInfo info = ev::arrayBufferInfo(part);
        if (info) out.insert(out.end(), info.data, info.data + info.byteLength);
        return;
    }
    if (ev::isUndefined(part) || ev::isNull(part)) return;
    // Anything else stringifies, which is what the web does — `new
    // Blob([42])` is the two bytes of "42".
    const std::string s = ev::toUtf8(part);
    out.insert(out.end(), s.begin(), s.end());
}

// `parts` is an array-like. It is walked through getProperty/getElement rather
// than any host-side array reader, because what the program passes is a real
// JS array and its elements may be getters.
std::vector<uint8_t> collectParts(Value partsValue) {
    std::vector<uint8_t> out;
    if (!ev::isObject(partsValue)) return out;

    ev::Persistent parts(partsValue);
    const double lenD = ev::toDouble(ev::getProperty(parts.get(), "length"));
    if (!(lenD > 0)) return out;
    const uint32_t len = static_cast<uint32_t>(lenD);
    for (uint32_t i = 0; i < len; ++i) {
        // Re-read through the Persistent each turn: getElement allocates, and
        // the array may have moved since the previous iteration.
        appendPart(out, ev::getElement(parts.get(), i));
    }
    return out;
}

std::string optionType(Value options) {
    if (!ev::isObject(options)) return {};
    Value t = ev::getProperty(options, "type");
    if (ev::isUndefined(t) || ev::isNull(t) || ev::isObject(t)) return {};
    return ev::toUtf8(t);
}

// ---------------------------------------------------------------------------
// base64, for readAsDataURL
// ---------------------------------------------------------------------------

std::string base64Encode(const std::vector<uint8_t>& in) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) |
                           uint32_t(in[i + 2]);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t n = uint32_t(in[i]) << 16;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// The Blob surface
// ---------------------------------------------------------------------------

// A promise already resolved with `v`. The reaction jobs land in the microtask
// queue the frame seam drains, so `await blob.text()` continues on the same
// turn the web would continue it on.
Value resolvedPromise(Value v) {
    ev::Persistent value(v);
    ev::Persistent p(ev::createPromise());
    ev::resolvePromise(p.get(), value.get());
    return p.get();
}

Value bytesToArrayBuffer(const std::vector<uint8_t>& bytes) {
    return ev::createArrayBuffer(std::span<const uint8_t>(bytes.data(), bytes.size()));
}

Value bytesToUint8Array(const std::vector<uint8_t>& bytes) {
    ev::Persistent view(ev::createTypedArray(ev::elements::Uint8,
                                             static_cast<uint32_t>(bytes.size())));
    // fillTypedArray does NOT allocate, so the view cannot have moved between
    // the two calls — but it is read back through the Persistent anyway,
    // because that invariant belongs to embed and not to this file.
    ev::fillTypedArray(view.get(), std::span<const uint8_t>(bytes.data(), bytes.size()));
    return view.get();
}

void installBlobSurface(ObjectBuilder& b, HostBlob* blob) {
    b.set("size", ev::fromDouble(static_cast<double>(blob->bytes.size())));
    b.set("type", ev::fromUtf8(blob->type));

    b.def("slice", 3, [blob](Value, std::span<const Value> a) {
        const double n = static_cast<double>(blob->bytes.size());
        // Negative offsets count from the end, as Array.prototype.slice does
        // and as the Blob spec spells out.
        auto clamp = [n](Value v, double dflt) {
            if (ev::isUndefined(v)) return dflt;
            double x = ev::toDouble(v);
            if (!(x == x)) return 0.0;  // NaN
            if (x < 0) x = n + x;
            return x < 0 ? 0.0 : (x > n ? n : x);
        };
        const double start = clamp(argAt(a, 0), 0.0);
        const double end = clamp(argAt(a, 1), n);
        Value typeV = argAt(a, 2);
        std::string type =
            (ev::isUndefined(typeV) || ev::isObject(typeV)) ? "" : ev::toUtf8(typeV);
        std::vector<uint8_t> cut;
        if (end > start) {
            cut.assign(blob->bytes.begin() + static_cast<ptrdiff_t>(start),
                       blob->bytes.begin() + static_cast<ptrdiff_t>(end));
        }
        return makeBlobValue(std::move(cut), std::move(type));
    });

    b.def("text", 0, [blob](Value, std::span<const Value>) {
        return resolvedPromise(ev::fromUtf8(
            std::string(blob->bytes.begin(), blob->bytes.end())));
    });
    b.def("arrayBuffer", 0, [blob](Value, std::span<const Value>) {
        return resolvedPromise(bytesToArrayBuffer(blob->bytes));
    });
    b.def("bytes", 0, [blob](Value, std::span<const Value>) {
        return resolvedPromise(bytesToUint8Array(blob->bytes));
    });
}

// ---------------------------------------------------------------------------
// FileReader
// ---------------------------------------------------------------------------

// Only what cannot live as a property: `readyState` is read by the program and
// so is an ordinary property, but the abort latch is written by abort() and
// read by a task that may already be queued, and a property read from inside
// the task would see whatever the program last assigned rather than what abort
// recorded.
struct HostReader {
    uint32_t tag = kHostReaderTag;  // must be first
    uint64_t generation = 0;        // bumped by abort() and by each new read
};

void hostReaderDtor(void* p) { delete static_cast<HostReader*>(p); }

HostReader* readerOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* r = static_cast<HostReader*>(ev::handleData(v));
    if (!r || r->tag != kHostReaderTag) return nullptr;
    return r;
}

// Set a property on an object held in a Persistent, keeping the Persistent
// current: setProperty may MOVE the object and answers its new address.
void setOn(ev::Persistent& obj, const char* key, Value v) {
    obj.set(ev::setProperty(obj.get(), key, v));
}

// The one shape every read takes: latch a generation, queue the copy, and on
// the way out either publish `result` and fire load/loadend, or publish `error`
// and fire error/loadend. `produce` runs on the task, so it allocates safely.
void startRead(Value self, Value blobValue,
               std::function<Value(const std::vector<uint8_t>&)> produce) {
    HostReader* reader = readerOf(self);
    if (!reader) return;
    const HostBlob* blob = hostBlobOf(blobValue);

    ev::Persistent target(self);
    setOn(target, "readyState", ev::fromDouble(1));  // LOADING
    setOn(target, "result", ev::null());
    setOn(target, "error", ev::null());

    const uint64_t generation = ++reader->generation;
    // A copy, not a reference: the Blob value is not rooted by this closure and
    // the read must survive the program dropping it. Blobs are immutable and
    // usually small enough that this is the honest cost of the interface.
    std::vector<uint8_t> bytes = blob ? blob->bytes : std::vector<uint8_t>();
    const bool haveBlob = blob != nullptr;

    postHostTask([target, generation, bytes = std::move(bytes), haveBlob,
                  produce = std::move(produce)]() mutable {
        ev::Persistent self2(target);
        HostReader* r = readerOf(self2.get());
        // abort(), or a second read started before this one ran. Either way
        // this task's result is stale and must not be published — the web's
        // rule that a reader delivers exactly one terminal event per read.
        if (!r || r->generation != generation) return;

        r->generation = generation;  // unchanged; kept explicit for the reader
        setOn(self2, "readyState", ev::fromDouble(2));  // DONE
        if (!haveBlob) {
            ObjectBuilder err;
            err.set("name", ev::fromUtf8("NotFoundError"));
            err.set("message", ev::fromUtf8("FileReader: argument is not a Blob"));
            setOn(self2, "error", err.get());
            dispatchHostEvent(ev::Persistent(self2.get()), "error");
            dispatchHostEvent(ev::Persistent(self2.get()), "loadend");
            return;
        }
        setOn(self2, "result", produce(bytes));
        dispatchHostEvent(ev::Persistent(self2.get()), "load");
        dispatchHostEvent(ev::Persistent(self2.get()), "loadend");
    });
}

Value makeFileReaderValue() {
    auto* reader = new HostReader();
    ObjectBuilder b(ev::makeHandle(reader, hostReaderDtor));

    // Data properties first, so the shape is fixed before a read rewrites
    // them — the same reason host_image.cpp seeds width/height up front.
    { Value z = ev::fromDouble(0); b.set("readyState", z); }
    { Value n = ev::null(); b.set("result", n); }
    { Value n = ev::null(); b.set("error", n); }
    for (const char* slot : {"onload", "onerror", "onloadend", "onloadstart",
                             "onprogress", "onabort"}) {
        Value n = ev::null();
        b.set(slot, n);
    }
    // The readyState constants, on the instance. On the web they are also on
    // FileReader itself — but a host function cannot carry a static at all (see
    // installFileGlobals), and `reader.DONE` is the spelling a compiled app
    // reaches for anyway.
    b.set("EMPTY", ev::fromDouble(0));
    b.set("LOADING", ev::fromDouble(1));
    b.set("DONE", ev::fromDouble(2));

    b.def("readAsText", 2, [](Value self, std::span<const Value> a) {
        // The encoding argument is accepted and ignored: the bytes a Blob holds
        // in this runtime came from UTF-8 sources, and a real transcoder here
        // would be a second, worse copy of the one brokit already has.
        startRead(self, argAt(a, 0), [](const std::vector<uint8_t>& bytes) {
            return ev::fromUtf8(std::string(bytes.begin(), bytes.end()));
        });
        return ev::undefined();
    });
    b.def("readAsArrayBuffer", 1, [](Value self, std::span<const Value> a) {
        startRead(self, argAt(a, 0), [](const std::vector<uint8_t>& bytes) {
            return bytesToArrayBuffer(bytes);
        });
        return ev::undefined();
    });
    b.def("readAsBinaryString", 1, [](Value self, std::span<const Value> a) {
        startRead(self, argAt(a, 0), [](const std::vector<uint8_t>& bytes) {
            // One character per BYTE, which is what the legacy method means —
            // not a UTF-8 decode.
            std::string s;
            s.reserve(bytes.size());
            for (uint8_t c : bytes) s += static_cast<char>(c);
            return ev::fromUtf8(s);
        });
        return ev::undefined();
    });
    b.def("readAsDataURL", 1, [](Value self, std::span<const Value> a) {
        const HostBlob* blob = hostBlobOf(argAt(a, 0));
        std::string mime = blob && !blob->type.empty() ? blob->type
                                                       : "application/octet-stream";
        startRead(self, argAt(a, 0),
                  [mime](const std::vector<uint8_t>& bytes) {
                      return ev::fromUtf8("data:" + mime + ";base64," +
                                          base64Encode(bytes));
                  });
        return ev::undefined();
    });
    b.def("abort", 0, [](Value self, std::span<const Value>) {
        HostReader* r = readerOf(self);
        if (!r) return ev::undefined();
        // Bumping the generation is the abort: the queued task finds a number
        // that is not its own and publishes nothing.
        ++r->generation;
        ev::Persistent target(self);
        setOn(target, "readyState", ev::fromDouble(2));
        setOn(target, "result", ev::null());
        dispatchHostEvent(ev::Persistent(target.get()), "abort");
        dispatchHostEvent(ev::Persistent(target.get()), "loadend");
        return ev::undefined();
    });

    b.def("addEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::undefined();
        addHostListener(self, ev::toUtf8(typeV), argAt(a, 1));
        return ev::undefined();
    });
    b.def("removeEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::undefined();
        removeHostListener(self, ev::toUtf8(typeV), argAt(a, 1));
        return ev::undefined();
    });

    return b.get();
}

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
    std::string href, protocol, hostname, port, pathname, search, hash;
};

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
        if (at != std::string::npos) authority = authority.substr(at + 1);
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

ParsedURL parseURL(const std::string& input, const std::string& base) {
    ParsedURL out;
    if (splitAbsolute(input, out)) {
        out.pathname = normalizePath(out.pathname);
    } else if (!base.empty()) {
        ParsedURL b;
        if (!splitAbsolute(base, b)) {
            out.pathname = input;
        } else {
            out.protocol = b.protocol;
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
        }
    } else {
        out.pathname = input;
    }

    out.href = out.protocol;
    if (!out.hostname.empty()) {
        out.href += "//" + out.hostname;
        if (!out.port.empty()) out.href += ":" + out.port;
    }
    out.href += out.pathname + out.search + out.hash;
    return out;
}

Value makeURLValue(const ParsedURL& u) {
    ObjectBuilder b;
    b.set("href", ev::fromUtf8(u.href));
    b.set("protocol", ev::fromUtf8(u.protocol));
    b.set("hostname", ev::fromUtf8(u.hostname));
    b.set("port", ev::fromUtf8(u.port));
    b.set("host", ev::fromUtf8(u.port.empty() ? u.hostname
                                              : u.hostname + ":" + u.port));
    b.set("pathname", ev::fromUtf8(u.pathname));
    b.set("search", ev::fromUtf8(u.search));
    b.set("hash", ev::fromUtf8(u.hash));
    b.set("origin", ev::fromUtf8(u.hostname.empty()
                                     ? std::string("null")
                                     : u.protocol + "//" + u.hostname +
                                           (u.port.empty() ? "" : ":" + u.port)));

    // searchParams, as a snapshot rather than a live view. That was forced
    // when the embed API had no property trap; it no longer is — makeHostProxy
    // (host_proxy.cpp) is what `dataset` is built on and would serve a live
    // URLSearchParams too. It stays a snapshot until someone needs the live
    // one, because a URLSearchParams whose set() did not write back to the URL
    // would be worse than one that plainly reads. get/has/getAll cover what a
    // library asks of it.
    {
        std::vector<std::pair<std::string, std::string>> pairs;
        if (u.search.size() > 1) {
            std::string q = u.search.substr(1);
            size_t i = 0;
            while (i <= q.size()) {
                size_t amp = q.find('&', i);
                if (amp == std::string::npos) amp = q.size();
                const std::string item = q.substr(i, amp - i);
                if (!item.empty()) {
                    const size_t eq = item.find('=');
                    if (eq == std::string::npos)
                        pairs.emplace_back(item, "");
                    else
                        pairs.emplace_back(item.substr(0, eq), item.substr(eq + 1));
                }
                i = amp + 1;
            }
        }
        ObjectBuilder sp;
        sp.def("get", 1, [pairs](Value, std::span<const Value> a) {
            const std::string key = ev::toUtf8(argAt(a, 0));
            for (const auto& kv : pairs)
                if (kv.first == key) return ev::fromUtf8(kv.second);
            return ev::null();
        });
        sp.def("has", 1, [pairs](Value, std::span<const Value> a) {
            const std::string key = ev::toUtf8(argAt(a, 0));
            for (const auto& kv : pairs)
                if (kv.first == key) return ev::fromBool(true);
            return ev::fromBool(false);
        });
        sp.def("getAll", 1, [pairs](Value, std::span<const Value> a) {
            const std::string key = ev::toUtf8(argAt(a, 0));
            std::vector<std::string> hits;
            for (const auto& kv : pairs)
                if (kv.first == key) hits.push_back(kv.second);
            return hostArrayOf(hits.size(),
                               [&hits](size_t i) { return ev::fromUtf8(hits[i]); });
        });
        b.set("searchParams", sp.get());
    }

    const std::string href = u.href;
    b.def("toString", 0, [href](Value, std::span<const Value>) {
        return ev::fromUtf8(href);
    });
    return b.get();
}

}  // namespace

// ---------------------------------------------------------------------------
// The pieces other files use
// ---------------------------------------------------------------------------

const HostBlob* hostBlobOf(Value v) { return mutableHostBlob(v); }

Value makeBlobValue(std::vector<uint8_t> bytes, std::string type) {
    auto* blob = new HostBlob();
    blob->bytes = std::move(bytes);
    blob->type = std::move(type);
    ObjectBuilder b(ev::makeHandle(blob, hostBlobDtor));
    installBlobSurface(b, blob);
    return b.get();
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installFileGlobals() {
    ev::registerGlobal("Blob", ev::makeFunction(
        [](Value, std::span<const Value> a) {
            std::vector<uint8_t> bytes = collectParts(argAt(a, 0));
            return makeBlobValue(std::move(bytes), optionType(argAt(a, 1)));
        },
        2));

    ev::registerGlobal("File", ev::makeFunction(
        [](Value, std::span<const Value> a) {
            auto* blob = new HostBlob();
            blob->bytes = collectParts(argAt(a, 0));
            blob->isFile = true;
            Value nameV = argAt(a, 1);
            blob->name = (ev::isObject(nameV) || ev::isUndefined(nameV))
                             ? "" : ev::toUtf8(nameV);
            Value options = argAt(a, 2);
            blob->type = optionType(options);
            if (ev::isObject(options)) {
                Value lm = ev::getProperty(options, "lastModified");
                if (!ev::isUndefined(lm)) blob->lastModified = ev::toDouble(lm);
            }

            ObjectBuilder b(ev::makeHandle(blob, hostBlobDtor));
            installBlobSurface(b, blob);
            b.set("name", ev::fromUtf8(blob->name));
            b.set("lastModified", ev::fromDouble(blob->lastModified));
            // webkitRelativePath is empty for a File the program built, and
            // present because file-input code reads it unconditionally.
            b.set("webkitRelativePath", ev::fromUtf8(""));
            return b.get();
        },
        3));

    ev::registerGlobal("FileReader", ev::makeFunction(
        [](Value, std::span<const Value>) { return makeFileReaderValue(); }, 0));

    // URL IS A NAMESPACE HERE, NOT A CONSTRUCTOR — `new URL(href)` does not
    // work and `URL.parse(href, base)` does.
    //
    // On the web URL is both: callable, and carrying createObjectURL. This was
    // once unbuildable — embed::setProperty called fatal() on any receiver that
    // was not a plain object, and the way round it (the program's own
    // Object.assign) was a hard runtime error rather than a catchable throw, so
    // there was no probing at startup and degrading. That is fixed: setProperty
    // takes a FUNCTION receiver now, landing the definition where a class
    // `static` member's would, so the callable-URL-with-statics shape is
    // available whenever this is converted.
    //
    // It has not been, because the namespace costs almost nothing. The statics
    // are the half that cannot be spelled any other way — createObjectURL is
    // why this family exists — and they work as plain properties of a plain
    // object. The constructor has an exact standard equivalent in `URL.parse`,
    // a real 2024 addition to the web platform that answers null instead of
    // throwing. What converting WOULD buy is `x instanceof URL`, and that is
    // precisely what it cannot buy: `prototype` stays refused by name.
    ObjectBuilder ns;
    ns.set("createObjectURL", makeCreateObjectURL());
    ns.set("revokeObjectURL", makeRevokeObjectURL());
    ns.def("parse", 2, [](Value, std::span<const Value> a) {
        Value hrefV = argAt(a, 0);
        if (ev::isObject(hrefV) || ev::isUndefined(hrefV)) return ev::null();
        Value baseV = argAt(a, 1);
        const std::string base =
            (ev::isObject(baseV) || ev::isUndefined(baseV)) ? "" : ev::toUtf8(baseV);
        return makeURLValue(parseURL(ev::toUtf8(hrefV), base));
    });
    ev::registerGlobal("URL", ns.get());
}

}  // namespace bro::bronze_host

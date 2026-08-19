// Blob, File, FileReader, and URL — bytes an app holds, and the names it gives
// them.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "util/log.h"
#include "util/object_url.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>
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

// The per-instance STATE of a Blob. Everything else a Blob can do is the same
// for every Blob and lives on the prototype below.
void installBlobState(ObjectBuilder& b, const HostBlob* blob) {
    b.set("size", ev::fromDouble(static_cast<double>(blob->bytes.size())));
    b.set("type", ev::fromUtf8(blob->type));
}

// The Blob METHODS, decorated once onto Blob.prototype. Each unwraps its
// RECEIVER rather than closing over a HostBlob* — which is not only tidier: a
// closure holding the raw payload of a cell it does not root is a dangling
// read the moment a detached method outlives its object, and every one of
// these used to be written that way.
void decorateBlobProto(ObjectBuilder& b) {
    b.def("slice", 3, [](Value self, std::span<const Value> a) {
        const HostBlob* blob = mutableHostBlob(self);
        if (!blob) return ev::throwTypeError("Blob.slice: the receiver is not a Blob");
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

    b.def("text", 0, [](Value self, std::span<const Value>) {
        const HostBlob* blob = mutableHostBlob(self);
        if (!blob) return ev::throwTypeError("Blob.text: the receiver is not a Blob");
        return resolvedPromise(ev::fromUtf8(
            std::string(blob->bytes.begin(), blob->bytes.end())));
    });
    b.def("arrayBuffer", 0, [](Value self, std::span<const Value>) {
        const HostBlob* blob = mutableHostBlob(self);
        if (!blob) {
            return ev::throwTypeError("Blob.arrayBuffer: the receiver is not a Blob");
        }
        return resolvedPromise(bytesToArrayBuffer(blob->bytes));
    });
    b.def("bytes", 0, [](Value self, std::span<const Value>) {
        const HostBlob* blob = mutableHostBlob(self);
        if (!blob) return ev::throwTypeError("Blob.bytes: the receiver is not a Blob");
        return resolvedPromise(bytesToUint8Array(blob->bytes));
    });
}

// The three classes this file installs. File EXTENDS Blob, as on the web.
HostClass g_blobClass;
HostClass g_fileClass;
HostClass g_readerClass;

// ---------------------------------------------------------------------------
// FileReader
// ---------------------------------------------------------------------------

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

        r->generation = generation;
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
    ObjectBuilder b(g_readerClass.make(reader, hostReaderDtor));

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
    return b.get();
}

// Everything a FileReader can DO, decorated once onto FileReader.prototype.
void decorateReaderProto(ObjectBuilder& b) {
    // The readyState constants sit on the prototype AND on the constructor,
    // which is where the web has them (`reader.DONE` and `FileReader.DONE`
    // both work); they are the same for every reader, so neither copy is
    // per-instance state.
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

void rebuildUrlHref(ParsedURL& u) {
    u.href = u.protocol;
    if (!u.hostname.empty()) {
        u.href += "//" + u.hostname;
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

HostClass g_urlClass;

// Bound to the PARSE, not to the URL object. `url.searchParams` is reachable
// on its own — a program may keep it and drop the URL — and the closures below
// would then be reading a HostUrl the finalizer has freed. Holding the
// shared_ptr keeps that case coherent: the detached view still reads and writes
// the same query it was made from.
Value makeSearchParamsObject(const std::shared_ptr<ParsedURL>& parsed) {
    ObjectBuilder sp;
    sp.def("get", 1, [parsed](Value, std::span<const Value> a) {
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(parsed->search);
        for (const auto& kv : pairs)
            if (kv.first == key) return ev::fromUtf8(kv.second);
        return ev::null();
    });
    sp.def("has", 1, [parsed](Value, std::span<const Value> a) {
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(parsed->search);
        for (const auto& kv : pairs)
            if (kv.first == key) return ev::fromBool(true);
        return ev::fromBool(false);
    });
    sp.def("getAll", 1, [parsed](Value, std::span<const Value> a) {
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(parsed->search);
        std::vector<std::string> hits;
        for (const auto& kv : pairs)
            if (kv.first == key) hits.push_back(kv.second);
        return hostArrayOf(hits.size(),
                           [&hits](size_t i) { return ev::fromUtf8(hits[i]); });
    });
    sp.def("set", 2, [parsed](Value, std::span<const Value> a) {
        std::string key = ev::toUtf8(argAt(a, 0));
        std::string val = ev::toUtf8(argAt(a, 1));
        auto pairs = parseQueryParams(parsed->search);
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
        parsed->search = q.empty() ? "" : "?" + q;
        rebuildUrlHref(*parsed);
        return ev::undefined();
    });
    sp.def("append", 2, [parsed](Value, std::span<const Value> a) {
        std::string key = ev::toUtf8(argAt(a, 0));
        std::string val = ev::toUtf8(argAt(a, 1));
        auto pairs = parseQueryParams(parsed->search);
        pairs.emplace_back(key, val);
        std::string q = serializeQueryParams(pairs);
        parsed->search = q.empty() ? "" : "?" + q;
        rebuildUrlHref(*parsed);
        return ev::undefined();
    });
    sp.def("delete", 1, [parsed](Value, std::span<const Value> a) {
        std::string key = ev::toUtf8(argAt(a, 0));
        auto pairs = parseQueryParams(parsed->search);
        std::vector<std::pair<std::string, std::string>> next;
        for (const auto& kv : pairs) {
            if (kv.first != key) next.push_back(kv);
        }
        std::string q = serializeQueryParams(next);
        parsed->search = q.empty() ? "" : "?" + q;
        rebuildUrlHref(*parsed);
        return ev::undefined();
    });
    sp.def("toString", 0, [parsed](Value, std::span<const Value>) {
        auto pairs = parseQueryParams(parsed->search);
        return ev::fromUtf8(serializeQueryParams(pairs));
    });
    return sp.get();
}

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
                   ev::Persistent made(makeSearchParamsObject(u->parsed));
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

// The MIME type a filename implies. There is no sniffing here and no content
// negotiation: an extension is all a dropped path carries, and it is what the
// interpreted realm's `__bro_fileFromPath` uses too (src/js/js/file_polyfills.js),
// so a File built here and one built there describe the same file the same way.
std::string mimeForName(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> kTable[] = {
        {"png", "image/png"},    {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},    {"webp", "image/webp"}, {"bmp", "image/bmp"},
        {"svg", "image/svg+xml"},
        {"json", "application/json"}, {"js", "text/javascript"},
        {"mjs", "text/javascript"},   {"css", "text/css"},
        {"html", "text/html"},   {"txt", "text/plain"},  {"md", "text/plain"},
        {"wav", "audio/wav"},    {"mp3", "audio/mpeg"},  {"ogg", "audio/ogg"},
        {"webm", "video/webm"},  {"mp4", "video/mp4"},
        {"glb", "model/gltf-binary"}, {"gltf", "model/gltf+json"},
        {"zip", "application/zip"},   {"wasm", "application/wasm"},
    };
    for (const auto& [k, v] : kTable)
        if (ext == k) return v;
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// The pieces other files use
// ---------------------------------------------------------------------------

Value makeFileFromPath(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path fsPath(path);

    std::ifstream in(fsPath, std::ios::binary);
    if (!in) return ev::undefined();
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (in.bad()) return ev::undefined();

    auto* blob = new HostBlob();
    blob->bytes = std::move(bytes);
    blob->isFile = true;
    blob->name = fsPath.filename().string();
    blob->type = mimeForName(blob->name);
    // Best effort, and zero when the clock is unreadable: `lastModified` is
    // metadata a drop handler may print, never something it branches on.
    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    if (!ec) {
        blob->lastModified = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                mtime.time_since_epoch()).count());
    }

    ObjectBuilder b(g_fileClass.make(blob, hostBlobDtor));
    installBlobState(b, blob);
    b.set("name", ev::fromUtf8(blob->name));
    b.set("lastModified", ev::fromDouble(blob->lastModified));
    b.set("webkitRelativePath", ev::fromUtf8(""));
    // Where it came from. Not a web property, and deliberately kept: a drop is
    // the one moment a page is handed a real filesystem path (docs/paths-api.js),
    // and the interpreted realm hands one over too.
    b.set("path", ev::fromUtf8(path));
    return b.get();
}
// ---------------------------------------------------------------------------

const HostBlob* hostBlobOf(Value v) { return mutableHostBlob(v); }

Value makeBlobValue(std::vector<uint8_t> bytes, std::string type) {
    auto* blob = new HostBlob();
    blob->bytes = std::move(bytes);
    blob->type = std::move(type);
    ObjectBuilder b(g_blobClass.make(blob, hostBlobDtor));
    installBlobState(b, blob);
    return b.get();
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installFileGlobals() {
    g_blobClass.install(
        "Blob", 2,
        [](Value, std::span<const Value> a) {
            std::vector<uint8_t> bytes = collectParts(argAt(a, 0));
            return makeBlobValue(std::move(bytes), optionType(argAt(a, 1)));
        },
        decorateBlobProto);

    g_fileClass.install(
        "File", 3,
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

            ObjectBuilder b(g_fileClass.make(blob, hostBlobDtor));
            installBlobState(b, blob);
            b.set("name", ev::fromUtf8(blob->name));
            b.set("lastModified", ev::fromDouble(blob->lastModified));
            // webkitRelativePath is empty for a File the program built, and
            // present because file-input code reads it unconditionally.
            b.set("webkitRelativePath", ev::fromUtf8(""));
            return b.get();
        },
        // A File carries no methods of its own: it inherits Blob's, through
        // the chain below.
        nullptr);
    // `file instanceof Blob` is true on the web, and a File really does answer
    // slice/text/arrayBuffer. One chain buys both.
    g_fileClass.inherit(g_blobClass);

    g_readerClass.install(
        "FileReader", 0,
        [](Value, std::span<const Value>) { return makeFileReaderValue(); },
        decorateReaderProto);
    // The web has the constants on the constructor too.
    g_readerClass.setStatic("EMPTY", ev::fromDouble(0));
    g_readerClass.setStatic("LOADING", ev::fromDouble(1));
    g_readerClass.setStatic("DONE", ev::fromDouble(2));

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
}

}  // namespace bro::bronze_host

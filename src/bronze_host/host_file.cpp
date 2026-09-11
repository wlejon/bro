// Blob, File, and FileReader — bytes an app holds, and the names it gives
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

// The MIME type a filename implies. There is no sniffing here and no content
// negotiation: an extension is all a dropped path carries, and it describes
// the MIME type uniformly.
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


    installUrlGlobals();
}

}  // namespace bro::bronze_host

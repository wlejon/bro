// The structured-clone WRITER (HTML StructuredSerializeInternal) behind
// postMessage, window.open's channel and bro.net's sendClone. The format is
// in host_worker_msg_format.h; the reader is host_worker_msg_read.cpp.
//
// Built on bronze's clone primitives (embed/embed_clone.h): a value's kind is
// a brand check, a plain object's properties are read straight out of its
// slots along a layout cached per shape, a Map's entries straight out of its
// table. Nothing on the common path calls into the program or allocates on
// the JS heap, so the only roots are the memory's, one per object.

#include "bronze_host/host_worker_msg.h"
#include "bronze_host/host_worker_msg_format.h"
#include "bronze_host/host_builder.h"
#include "abi/bronze_abi.h"
#include "embed/embed_clone.h"
#include "runtime/heap.h"
#if BRO_WITH_3D
#include <bromesh/api.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace bro::bronze_host {

using namespace clonefmt;
namespace clone = bronze::embed::clone;

ByteBlock::ByteBlock(const uint8_t* src, uint32_t n) : size(n) {
    if (n == 0) return;
    data = static_cast<uint8_t*>(std::malloc(n));
    if (!data) {
        size = 0;
        ev::throwRangeError("postMessage: out of memory copying an ArrayBuffer");
    }
    if (src) std::memcpy(data, src, n);
    else std::memset(data, 0, n);
}

ByteBlock& ByteBlock::operator=(ByteBlock&& o) noexcept {
    if (this != &o) {
        std::free(data);
        data = o.data;
        size = o.size;
        o.data = nullptr;
        o.size = 0;
    }
    return *this;
}

ByteBlock::~ByteBlock() { std::free(data); }

namespace {

using TransferRoots = std::vector<ev::Persistent>;

// The transfer list is compared by the roots' current values: a raw Value
// copied at entry would name a pre-collection address after the first
// allocation, and a listed buffer would be copied instead of transferred.
bool isTransferred(Value v, const TransferRoots& transfers) {
    for (const ev::Persistent& t : transfers) {
        if (t.get().rawBits() == v.rawBits()) return true;
    }
    return false;
}

// The serialization memory: every object written so far, numbered in the
// order written, held in roots. The address index is an open-addressed
// table rebuilt whenever the collector has moved objects since it was built
// (relocationEpoch), so a probe never answers a stale address.
class ObjectMemory {
public:
    int64_t find(Value v) {
        refresh();
        if (count_ == 0) return -1;
        const uint64_t bits = v.rawBits();
        for (size_t i = hash(bits) & mask_;; i = (i + 1) & mask_) {
            if (keys_[i] == 0) return -1;
            if (keys_[i] == bits) return vals_[i];
        }
    }
    uint32_t add(Value v) {
        refresh();
        const uint32_t idx = static_cast<uint32_t>(roots_.size());
        roots_.emplace_back(v);
        insert(v.rawBits(), idx);
        return idx;
    }
    Value at(uint32_t idx) const { return roots_[idx].get(); }

private:
    static size_t hash(uint64_t bits) { return static_cast<size_t>((bits >> 3) * 0x9E3779B97F4A7C15ull >> 20); }
    void insert(uint64_t bits, uint32_t idx) {
        if ((count_ + 1) * 2 > keys_.size()) grow();
        size_t i = hash(bits) & mask_;
        while (keys_[i] != 0 && keys_[i] != bits) i = (i + 1) & mask_;
        if (keys_[i] == 0) ++count_;
        keys_[i] = bits;
        vals_[i] = idx;
    }
    void grow() {
        const size_t cap = keys_.empty() ? 64 : keys_.size() * 2;
        keys_.assign(cap, 0);
        vals_.assign(cap, 0);
        mask_ = cap - 1;
        count_ = 0;
        for (size_t i = 0; i < roots_.size(); ++i) insert(roots_[i].get().rawBits(), static_cast<uint32_t>(i));
    }
    void refresh() {
        const uint64_t epoch = ev::relocationEpoch();
        if (epoch == epoch_) return;
        epoch_ = epoch;
        std::fill(keys_.begin(), keys_.end(), 0);
        count_ = 0;
        for (size_t i = 0; i < roots_.size(); ++i) insert(roots_[i].get().rawBits(), static_cast<uint32_t>(i));
    }
    std::vector<ev::Persistent> roots_;
    std::vector<uint64_t> keys_;
    std::vector<uint32_t> vals_;
    size_t mask_ = 0;
    size_t count_ = 0;
    uint64_t epoch_ = ev::relocationEpoch();
};

class Out {
public:
    explicit Out(std::vector<uint8_t>& b) : b_(b) { b_.reserve(4096); }
    void u8(uint8_t v) { b_.push_back(v); }
    void u32(uint32_t v) { std::memcpy(grow(4), &v, 4); }
    void f64(double v) { std::memcpy(grow(8), &v, 8); }
    void bytes(const void* p, size_t n) { if (n) std::memcpy(grow(n), p, n); }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); bytes(s.data(), s.size()); }
    size_t size() const { return b_.size(); }
    void patch32(size_t at, uint32_t v) { std::memcpy(b_.data() + at, &v, 4); }
private:
    uint8_t* grow(size_t n) {
        const size_t o = b_.size();
        b_.resize(o + n);
        return b_.data() + o;
    }
    std::vector<uint8_t>& b_;
};

// Written in its own statement: the lookup may allocate (the builtin
// namespaces build lazily), so the object is read from its root after it.
bool isInstanceOf(const ObjectMemory& mem, uint32_t m, const char* ctorName) {
    auto g = ev::globalValue(ctorName);
    if (!g.found || !ev::isObject(g.value)) return false;
    return bronze_instanceof(mem.at(m).rawBits(), g.value.rawBits());
}

std::string keyUtf8(clone::Key k) {
    const clone::Chars c = clone::keyChars(k);
    std::string s;
    if (!c.utf16) {
        const auto* p = static_cast<const uint8_t*>(c.data);
        for (uint32_t i = 0; i < c.length; ++i) {
            if (p[i] < 0x80) s.push_back(static_cast<char>(p[i]));
            else { s.push_back(static_cast<char>(0xC0 | (p[i] >> 6))); s.push_back(static_cast<char>(0x80 | (p[i] & 0x3F))); }
        }
        return s;
    }
    return ev::toUtf8(clone::keyValue(k));
}

class Writer {
public:
    Writer(Message& msg, const TransferRoots& transfers, CloneTarget target)
        : msg_(msg), out_(msg.data), transfers_(transfers), target_(target) {}

    bool value(Value val, int depth);

private:
    struct Layout {
        uint32_t id = 0;
        bool generic = false;          // an enumerable accessor: the entries path
        const char* refusal = nullptr;  // a DOM Node
        std::vector<clone::PropSlot> props;
    };

    bool object(Value val, int depth);
    bool plain(uint32_t m, int depth);
    bool props(uint32_t m, const void* token, const std::vector<clone::PropSlot>& props, int depth);
    bool generic(uint32_t m, int depth);
    bool array(uint32_t m, int depth);
    bool collection(uint32_t m, bool isMap, int depth);
    bool handle(uint32_t m);
    void buffer(Value buf);
    void string(Value s);
    void key(clone::Key k);
    void chars(const clone::Chars& c);

    Message& msg_;
    Out out_;
    const TransferRoots& transfers_;
    CloneTarget target_;
    ObjectMemory mem_;
    std::unordered_map<const void*, Layout> layouts_;  // by shape token; references stay valid
    std::unordered_map<const void*, uint32_t> keyIds_;
    uint32_t nextLayout_ = 0;
};

void Writer::chars(const clone::Chars& c) {
    out_.u8(c.utf16 ? 1 : 0);
    out_.u32(c.length);
    out_.bytes(c.data, size_t(c.length) * (c.utf16 ? 2 : 1));
}

void Writer::key(clone::Key k) {
    auto [it, fresh] = keyIds_.try_emplace(k, static_cast<uint32_t>(keyIds_.size()));
    out_.u32(it->second);
    if (fresh) chars(clone::keyChars(k));
}

void Writer::string(Value s) {
    const clone::Chars c = clone::stringChars(s);
    out_.u8(c.utf16 ? kStringUtf16 : kStringLatin1);
    out_.u32(c.length);
    out_.bytes(c.data, size_t(c.length) * (c.utf16 ? 2 : 1));
}

void Writer::buffer(Value buf) {
    auto info = ev::arrayBufferInfo(buf);
    const bool transferred = isTransferred(buf, transfers_);
    if (transferred || (target_ == CloneTarget::Local && info.byteLength >= kOutOfBandMin)) {
        // Out of band: one copy here, none on the receiving side, which
        // adopts the block as an external ArrayBuffer. A transferred buffer
        // is detached by serializeMessage once the whole value is written,
        // since a view later in the payload still reads it.
        const uint32_t idx = static_cast<uint32_t>(msg_.buffers.size());
        msg_.buffers.emplace_back(info.data, info.data ? info.byteLength : 0);
        out_.u8(kBufferBlock);
        out_.u32(idx);
        return;
    }
    out_.u8(kArrayBuffer);
    out_.u32(info.byteLength);
    if (info.data) out_.bytes(info.data, info.byteLength);
}

bool Writer::value(Value val, int depth) {
    if (depth > kMaxDepth) {
        ev::throwTypeError("postMessage: object too deeply nested");
        return false;
    }
    if (val.isNumber()) {
        const double d = val.asNumber();
        if (d >= -2147483648.0 && d <= 2147483647.0 && double(int32_t(d)) == d &&
            !(d == 0 && std::signbit(d))) {
            out_.u8(kInt32);
            out_.u32(static_cast<uint32_t>(int32_t(d)));
        } else {
            out_.u8(kFloat64);
            out_.f64(d);
        }
        return true;
    }
    if (ev::isString(val)) { string(val); return true; }
    if (ev::isUndefined(val)) { out_.u8(kUndefined); return true; }
    if (ev::isNull(val))      { out_.u8(kNull);      return true; }
    if (ev::isBool(val))      { out_.u8(ev::toBool(val) ? kTrue : kFalse); return true; }
    if (ev::isBigInt(val)) {
        out_.u8(kBigInt);
        out_.str(ev::toUtf8(val));
        return true;
    }
    if (ev::isObject(val)) return object(val, depth);
    ev::throwTypeError("postMessage: value is not cloneable");
    return false;
}

bool Writer::object(Value val, int depth) {
    // Numbered on first sight, before anything reads it (and before its
    // children are written, so a cycle back to it finds it); a reference
    // from then on.
    const int64_t seen = mem_.find(val);
    if (seen >= 0) {
        out_.u8(kObjectRef);
        out_.u32(static_cast<uint32_t>(seen));
        return true;
    }
    const clone::Kind kind = clone::classify(val);
    if (kind == clone::Kind::Function) {
        ev::throwTypeError("postMessage: function is not cloneable");
        return false;
    }
    if (kind == clone::Kind::Promise) {
        ev::throwTypeError("postMessage: Promise is not cloneable");
        return false;
    }
    if (kind == clone::Kind::Weak) {
        ev::throwTypeError("postMessage: weak collections are not cloneable");
        return false;
    }
    const uint32_t m = mem_.add(val);

    switch (kind) {
    case clone::Kind::Plain: return plain(m, depth);
    case clone::Kind::Array: return array(m, depth);
    case clone::Kind::Map: return collection(m, true, depth);
    case clone::Kind::Set: return collection(m, false, depth);
    case clone::Kind::Date:
        out_.u8(kDate);
        out_.f64(clone::dateValue(val));
        return true;
    case clone::Kind::ArrayBuffer:
        buffer(val);
        return true;
    case clone::Kind::TypedArray: {
        auto info = ev::typedArrayInfo(val);
        const uint8_t subtype = static_cast<uint8_t>(info.elementKind);
        const uint32_t viewBytes = info.byteLength;
        // Everything read off the view comes before typedArrayBuffer, which
        // may materialize the buffer object (an allocation that moves `val`).
        const uint32_t offset = ev::typedArrayByteOffset(val);
        Value bufVal = ev::typedArrayBuffer(val);
        out_.u8(kTypedArray);
        out_.u8(subtype);
        out_.u32(offset);
        out_.u32(viewBytes);
        return value(bufVal, depth + 1);
    }
    case clone::Kind::DataView: {
        const uint32_t off = satCast<uint32_t>(ev::toDouble(ev::getProperty(mem_.at(m), "byteOffset")));
        const uint32_t viewBytes = satCast<uint32_t>(ev::toDouble(ev::getProperty(mem_.at(m), "byteLength")));
        Value buf = ev::getProperty(mem_.at(m), "buffer");
        out_.u8(kDataView);
        out_.u32(off);
        out_.u32(viewBytes);
        return value(buf, depth + 1);
    }
    case clone::Kind::RegExp: {
        std::string src = ev::toUtf8(ev::getProperty(mem_.at(m), "source"));
        std::string flags = ev::toUtf8(ev::getProperty(mem_.at(m), "flags"));
        out_.u8(kRegExp);
        out_.str(src);
        out_.str(flags);
        return true;
    }
    case clone::Kind::Error: {
        std::string name = ev::toUtf8(ev::getProperty(mem_.at(m), "name"));
        std::string message = ev::toUtf8(ev::getProperty(mem_.at(m), "message"));
        std::string stack = ev::toUtf8(ev::getProperty(mem_.at(m), "stack"));
        out_.u8(kError);
        out_.str(name);
        out_.str(message);
        out_.str(stack);
        return true;
    }
    case clone::Kind::Handle: return handle(m);
    default: return generic(m, depth);  // a Proxy, or an exotic the primitives do not read
    }
}

bool Writer::handle(uint32_t m) {
    Value val = mem_.at(m);
    if (auto* bmp = hostImageBitmapOfMut(val)) {
        if (bmp->closed) {
            ev::throwTypeError("postMessage: ImageBitmap is closed");
            return false;
        }
        const uint32_t idx = static_cast<uint32_t>(msg_.transferredImages.size());
        // The pixels cross by reference, never by copy: a transfer moves the
        // sender's reference into the message (and detaches the sender), a
        // clone shares it — an ImageBitmap's pixels are immutable.
        SerializedImage simg;
        simg.width = bmp->width;
        simg.height = bmp->height;
        simg.pixels = bmp->pixels;
        if (isTransferred(val, transfers_)) {
            simg.upload = startTransferUpload(*bmp);
            bmp->detach();
        }
        msg_.transferredImages.push_back(std::move(simg));
        out_.u8(kTransferImageBitmap);
        out_.u32(idx);
        return true;
    }
#if BRO_WITH_3D
    // A Mesh is TRANSFERABLE and never cloned: its MeshData moves across by
    // pointer and the sender's handle is left empty, the way a transferred
    // ArrayBuffer is detached. Cloning one silently would copy what is often
    // megabytes of geometry, and a sendClone over the network has no realm to
    // receive a pointer.
    if (bromesh::api::isMeshValue(val)) {
        if (!isTransferred(val, transfers_)) {
            ev::throwTypeError("postMessage: Mesh must be listed in the transferList");
            return false;
        }
        auto data = std::make_unique<bromesh::MeshData>();
        bromesh::api::takeMeshData(val, *data);
        const uint32_t idx = static_cast<uint32_t>(msg_.transferredMeshes.size());
        msg_.transferredMeshes.push_back(std::move(data));
        out_.u8(kTransferMesh);
        out_.u32(idx);
        return true;
    }
#endif
    if (isInstanceOf(mem_, m, "Node")) {
        ev::throwTypeError("postMessage: DOM Nodes are not cloneable");
        return false;
    }
    // A pointer into this realm's engine state (SceneNode, GpuTensor, ...):
    // its properties all live on the prototype, so cloning it would hand the
    // receiver an empty object where it expected the resource.
    ev::throwTypeError("postMessage: native objects are not cloneable");
    return false;
}

bool Writer::plain(uint32_t m, int depth) {
    const void* token = clone::layoutToken(mem_.at(m));
    if (!token) {
        // A dictionary-mode object: its layout is its own, read each time.
        std::vector<clone::PropSlot> own;
        if (!clone::ownDataLayout(mem_.at(m), own)) return generic(m, depth);
        if (isInstanceOf(mem_, m, "Node")) {
            ev::throwTypeError("postMessage: DOM Nodes are not cloneable");
            return false;
        }
        out_.u8(kObject);
        out_.u32(static_cast<uint32_t>(own.size()));
        for (const clone::PropSlot& p : own) {
            key(p.key);
            // By name: a dictionary's slots are not a layout anything shares.
            if (!value(ev::getProperty(mem_.at(m), keyUtf8(p.key)), depth + 1)) return false;
        }
        return true;
    }

    auto [it, fresh] = layouts_.try_emplace(token);
    Layout& layout = it->second;
    if (fresh) {
        layout.generic = !clone::ownDataLayout(mem_.at(m), layout.props);
        // Once per shape: a shape's prototype is fixed, so the answer is too.
        if (isInstanceOf(mem_, m, "Node")) layout.refusal = "postMessage: DOM Nodes are not cloneable";
        if (!layout.generic && !layout.refusal) layout.id = nextLayout_++;
    }
    if (layout.refusal) {
        ev::throwTypeError(layout.refusal);
        return false;
    }
    if (layout.generic) return generic(m, depth);

    out_.u8(kObjectLayout);
    out_.u32(layout.id);
    if (fresh) {
        out_.u32(static_cast<uint32_t>(layout.props.size()));
        for (const clone::PropSlot& p : layout.props) key(p.key);
    }
    return props(m, token, layout.props, depth);
}

// The values of `list`, read from the object's slots one at a time from its
// root. `token` is the layout the slots were read under: if a getter run by
// a nested generic clone reshaped the object meanwhile, the rest are read by
// name (the program's [[Get]]) instead of from slots that may have moved.
bool Writer::props(uint32_t m, const void* token, const std::vector<clone::PropSlot>& list, int depth) {
    for (const clone::PropSlot& p : list) {
        Value obj = mem_.at(m);
        Value v = clone::layoutToken(obj) == token
                      ? clone::slotValue(obj, p.slot)
                      : ev::getProperty(obj, keyUtf8(p.key));
        if (!value(v, depth + 1)) return false;
    }
    return true;
}

bool Writer::array(uint32_t m, int depth) {
    const uint32_t len = clone::arrayLength(mem_.at(m));
    out_.u8(kArray);
    out_.u32(len);
    for (uint32_t i = 0; i < len; ++i) {
        // Past a length a getter shortened, an element reads as undefined.
        if (!value(clone::arrayElement(mem_.at(m), i), depth + 1)) return false;
    }
    return true;
}

bool Writer::collection(uint32_t m, bool isMap, int depth) {
    out_.u8(isMap ? kMap : kSet);
    const size_t countAt = out_.size();
    out_.u32(0);
    uint32_t count = 0;
    for (uint32_t i = 0; i < clone::collectionPositions(mem_.at(m)); ++i) {
        Value k, v;
        if (!clone::collectionEntry(mem_.at(m), i, k, v)) continue;
        if (!value(k, depth + 1)) return false;
        if (isMap) {
            // Re-read: writing the key may have allocated. A getter that
            // deleted the entry meanwhile leaves its value undefined.
            if (!clone::collectionEntry(mem_.at(m), i, k, v)) v = ev::undefined();
            if (!value(v, depth + 1)) return false;
        }
        ++count;
    }
    out_.patch32(countAt, count);
    return true;
}

// Whatever the primitives do not read directly — a Proxy, an exotic object,
// a plain object with an enumerable accessor — through the program's own
// operations, as the HTML algorithm spells them: IsArray, then
// EnumerableOwnProperties with [[Get]] (getters run).
bool Writer::generic(uint32_t m, int depth) {
    Value isArrFn = ev::getProperty(ev::globalValue("Array").value, "isArray");
    Value arrArg = mem_.at(m);
    Value isArr = ev::call(isArrFn, ev::undefined(), std::span<const Value>(&arrArg, 1)).value;
    if (ev::toBool(isArr)) {
        const uint32_t len = satCast<uint32_t>(ev::toDouble(ev::getProperty(mem_.at(m), "length")));
        out_.u8(kArray);
        out_.u32(len);
        for (uint32_t i = 0; i < len; ++i) {
            if (!value(ev::getElement(mem_.at(m), i), depth + 1)) return false;
        }
        return true;
    }
    Value entriesFn = ev::getProperty(ev::globalValue("Object").value, "entries");
    Value entriesArg = mem_.at(m);
    ev::Persistent entries(ev::call(entriesFn, ev::undefined(), std::span<const Value>(&entriesArg, 1)).value);
    const uint32_t n = satCast<uint32_t>(ev::toDouble(ev::getProperty(entries.get(), "length")));
    out_.u8(kObject);
    out_.u32(n);
    for (uint32_t i = 0; i < n; ++i) {
        ev::Persistent entry(ev::getElement(entries.get(), i));
        Value k = ev::getElement(entry.get(), 0);
        out_.u32(kLiteralKey);
        chars(clone::stringChars(k));
        if (!value(ev::getElement(entry.get(), 1), depth + 1)) return false;
    }
    return true;
}

}  // namespace

bool serializeMessage(Value val, std::span<const Value> transfers, Message& out, CloneTarget target) {
    // Rooted before anything allocates; the Persistent constructor itself
    // does not touch the JS heap.
    ev::Persistent root(val);
    TransferRoots transferRoots;
    transferRoots.reserve(transfers.size());
    for (Value t : transfers) transferRoots.emplace_back(t);

    out.data.clear();
    out.buffers.clear();
    out.transferredImages.clear();
#if BRO_WITH_3D
    out.transferredMeshes.clear();
#endif
    if (target == CloneTarget::Wire && !transferRoots.empty()) {
        ev::throwTypeError("postMessage: a transfer list cannot cross the network");
        return false;
    }
    Writer w(out, transferRoots, target);
    if (!w.value(root.get(), 0)) return false;
    // Serialize first, detach after (HTML StructuredSerializeWithTransfer):
    // every listed ArrayBuffer is detached, including one the payload reaches
    // only through a view, and none is when the clone fails.
    for (const ev::Persistent& t : transferRoots) {
        if (ev::isArrayBuffer(t.get())) ev::detachArrayBuffer(t.get());
    }
    return true;
}

std::vector<ev::Persistent> collectTransferList(std::span<const Value> args, size_t index) {
    std::vector<ev::Persistent> roots;
    // args[index] is a rooted argument slot, current across every read.
    if (args.size() <= index || !ev::isObject(args[index])) return roots;
    Value lenV = ev::getProperty(args[index], "length");
    if (!ev::isNumber(lenV)) return roots;
    const double len = ev::toDouble(lenV);
    if (!(len > 0)) return roots;
    const uint32_t n = static_cast<uint32_t>(std::min(len, 4294967295.0));
    for (uint32_t i = 0; i < n; ++i) {
        roots.emplace_back(ev::getElement(args[index], i));
    }
    return roots;
}

std::vector<Value> currentValues(const std::vector<ev::Persistent>& roots) {
    std::vector<Value> values;
    values.reserve(roots.size());
    for (const ev::Persistent& r : roots) values.push_back(r.get());
    return values;
}

}  // namespace bro::bronze_host

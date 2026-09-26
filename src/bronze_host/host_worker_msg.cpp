#include "bronze_host/host_worker_msg.h"
#include "bronze_host/gl_internal.h"
#include "abi/bronze_abi.h"
#include "runtime/heap.h"
#if BRO_WITH_3D
#include <bromesh/api.h>
#endif
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

// Takes the object as a root: the constructor lookup may allocate (the
// builtin namespaces build lazily), so the object is read only after it.
static bool isInstanceOf(const ev::Persistent& obj, const char* globalCtorName) {
    auto g = ev::globalValue(globalCtorName);
    if (!g.found || !ev::isObject(g.value)) return false;
    Value val = obj.get();
    if (!ev::isObject(val)) return false;
    return bronze_instanceof(val.rawBits(), g.value.rawBits());
}

enum Tag : uint8_t {
    kUndefined       = 0x00,
    kNull            = 0x01,
    kTrue            = 0x02,
    kFalse           = 0x03,
    kInt32           = 0x04,
    kFloat64         = 0x05,
    kString          = 0x06,
    kArray           = 0x07,
    kObject          = 0x08,
    kArrayBuffer     = 0x09,
    kTransferIndex   = 0x0A,
    kTypedArray      = 0x0B,
    kBigInt          = 0x0D,
    kTransferImageBitmap = 0x0E,
    kDate            = 0x0F,
    kRegExp          = 0x10,
    kMap             = 0x11,
    kSet             = 0x12,
    kError           = 0x13,
    kDataView        = 0x14,
    kTransferMesh    = 0x15,  // index into transferredMeshes (zero-copy Mesh)
    kObjectRef       = 0x16,  // an object already in this message, by order
};

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& buf) : buf_(buf) {}
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v) {
        buf_.push_back(v & 0xFF);
        buf_.push_back((v >> 8) & 0xFF);
        buf_.push_back((v >> 16) & 0xFF);
        buf_.push_back((v >> 24) & 0xFF);
    }
    void f64(double v) {
        uint8_t b[8];
        std::memcpy(b, &v, 8);
        buf_.insert(buf_.end(), b, b + 8);
    }
    void bytes(const uint8_t* p, size_t n) {
        buf_.insert(buf_.end(), p, p + n);
    }
private:
    std::vector<uint8_t>& buf_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool ok(size_t n = 0) const { return pos_ + n <= size_; }
    uint8_t u8() { return data_[pos_++]; }
    uint32_t u32() {
        uint32_t v = uint32_t(data_[pos_])
                   | (uint32_t(data_[pos_ + 1]) << 8)
                   | (uint32_t(data_[pos_ + 2]) << 16)
                   | (uint32_t(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    double f64() {
        double v;
        std::memcpy(&v, data_ + pos_, 8);
        pos_ += 8;
        return v;
    }
    const uint8_t* ptr(size_t n) {
        const uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }
private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

// The transfer list is held in roots for the whole clone, and compared by the
// roots' current values: a raw Value copied at entry would name a
// pre-collection address after the first allocation, and identity would then
// silently fail — a listed ArrayBuffer copied instead of transferred.
using TransferRoots = std::vector<ev::Persistent>;

static bool isTransferred(Value v, const TransferRoots& transfers) {
    for (const ev::Persistent& t : transfers) {
        if (t.get().rawBits() == v.rawBits()) return true;
    }
    return false;
}

static Value getGlobal(std::string_view name) {
    auto g = ev::globalValue(name);
    return g.found ? g.value : ev::undefined();
}

// The serialization "memory" of HTML StructuredSerializeInternal: every
// object written so far, numbered in the order written. An object reached a
// second time — the same object under two keys, two views over one buffer, a
// cycle back to an ancestor — is sent as a reference to its number, and the
// reader, numbering the objects it makes in the same order, hands back the
// one it made. Held in roots; the address index is rebuilt whenever the
// collector has moved objects since it was built (relocationEpoch), so a
// lookup is a hash probe and never goes stale.
class ObjectMemory {
public:
    // The number `v` was registered under, or -1.
    int64_t find(Value v) {
        refresh();
        auto it = index_.find(v.rawBits());
        return it == index_.end() ? -1 : static_cast<int64_t>(it->second);
    }
    // Registers `v` (current at the call) under the next number. Allocates
    // nothing on the JS heap.
    void add(Value v) {
        refresh();
        index_.emplace(v.rawBits(), static_cast<uint32_t>(roots_.size()));
        roots_.emplace_back(v);
    }
private:
    void refresh() {
        const uint64_t epoch = ev::relocationEpoch();
        if (epoch == epoch_) return;
        index_.clear();
        for (size_t i = 0; i < roots_.size(); ++i) {
            index_.emplace(roots_[i].get().rawBits(), static_cast<uint32_t>(i));
        }
        epoch_ = epoch;
    }
    std::vector<ev::Persistent> roots_;
    std::unordered_map<uint64_t, uint32_t> index_;
    uint64_t epoch_ = ev::relocationEpoch();
};

// One clone's state: the transfer list and the object memory.
struct CloneState {
    const TransferRoots& transfers;
    ObjectMemory memory;
};

static bool writeValue(Value val, Writer& w, CloneState& st,
                       Message& out, int depth);

// Writes an ArrayBuffer's bytes (or its transfer slot). The caller has
// registered it in the memory already.
static void writeBuffer(Value buf, Writer& w, CloneState& st, Message& out) {
    auto info = ev::arrayBufferInfo(buf);
    if (isTransferred(buf, st.transfers)) {
        auto& transferBufs = out.transferredBuffers;
        uint32_t idx = static_cast<uint32_t>(transferBufs.size());
        if (info.data && info.byteLength > 0) {
            transferBufs.emplace_back(info.data, info.data + info.byteLength);
        } else {
            transferBufs.emplace_back();
        }
        // Detached by serializeMessage once the whole value is written:
        // a view of this buffer later in the payload still reads it.
        w.u8(kTransferIndex);
        w.u32(idx);
        return;
    }
    w.u8(kArrayBuffer);
    w.u32(info.byteLength);
    if (info.data && info.byteLength > 0) w.bytes(info.data, info.byteLength);
}

// `val` is current at entry. Every branch that allocates before it is done
// with `val` works from a root of it (`self`), never from the parameter.
static bool writeValue(Value val, Writer& w, CloneState& st,
                       Message& out, int depth) {
    const TransferRoots& transfers = st.transfers;
    auto& transferImgs = out.transferredImages;
    if (depth > 64) {
        ev::throwTypeError("postMessage: object too deeply nested");
        return false;
    }

    if (ev::isUndefined(val)) { w.u8(kUndefined); return true; }
    if (ev::isNull(val))      { w.u8(kNull);      return true; }
    if (ev::isBool(val))      { w.u8(ev::toBool(val) ? kTrue : kFalse); return true; }

    if (ev::isNumber(val)) {
        double d = ev::toDouble(val);
        int64_t i64 = satCast<int64_t>(d);
        if (static_cast<double>(i64) == d && i64 >= INT32_MIN && i64 <= INT32_MAX) {
            w.u8(kInt32);
            w.u32(static_cast<uint32_t>(static_cast<int32_t>(i64)));
        } else {
            w.u8(kFloat64);
            w.f64(d);
        }
        return true;
    }

    if (ev::isBigInt(val)) {
        std::string s = ev::toUtf8(val);
        w.u8(kBigInt);
        w.u32(static_cast<uint32_t>(s.size()));
        w.bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return true;
    }

    if (ev::isString(val)) {
        std::string s = ev::toUtf8(val);
        w.u8(kString);
        w.u32(static_cast<uint32_t>(s.size()));
        w.bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return true;
    }

    if (ev::isFunction(val)) {
        ev::throwTypeError("postMessage: function is not cloneable");
        return false;
    }

    if (ev::isPromise(val)) {
        ev::throwTypeError("postMessage: Promise is not cloneable");
        return false;
    }

    // Every object is numbered on first sight, before anything below reads
    // it (and before its children are written, so a cycle back to it finds
    // it), and sent as a reference from then on.
    if (ev::isObject(val)) {
        const int64_t seen = st.memory.find(val);
        if (seen >= 0) {
            w.u8(kObjectRef);
            w.u32(static_cast<uint32_t>(seen));
            return true;
        }
        st.memory.add(val);
    }

    if (auto* bmp = hostImageBitmapOfMut(val)) {
        if (bmp->closed) {
            ev::throwTypeError("postMessage: ImageBitmap is closed");
            return false;
        }
        bool transferred = isTransferred(val, transfers);
        uint32_t idx = static_cast<uint32_t>(transferImgs.size());
        SerializedImage simg;
        simg.width = bmp->width;
        simg.height = bmp->height;
        if (transferred) {
            simg.pixels = std::move(bmp->pixels);
            bmp->closed = true;
            bmp->width = 0;
            bmp->height = 0;
            bmp->image = nullptr;
        } else {
            simg.pixels = bmp->pixels;
        }
        transferImgs.push_back(std::move(simg));

        w.u8(kTransferImageBitmap);
        w.u32(idx);
        return true;
    }

    if (ev::isArrayBuffer(val)) {
        writeBuffer(val, w, st, out);
        return true;
    }

    if (ev::isTypedArray(val)) {
        auto info = ev::typedArrayInfo(val);
        uint8_t subtype = static_cast<uint8_t>(info.elementKind);
        const uint32_t viewBytes = info.byteLength;
        // Everything read off the view comes before typedArrayBuffer, which
        // may materialize the buffer object (an allocation that moves `val`),
        // and the buffer is written straight after it.
        uint32_t offset = ev::typedArrayByteOffset(val);
        Value bufVal = ev::typedArrayBuffer(val);

        w.u8(kTypedArray);
        w.u8(subtype);
        w.u32(offset);
        w.u32(viewBytes);
        return writeValue(bufVal, w, st, out, depth + 1);
    }

    if (ev::isFunction(val)) {
        ev::throwTypeError("postMessage: functions are not cloneable");
        return false;
    }

    if (ev::isObject(val)) {
        const auto* hdr = val.asObject<bronze::HeapObjectHeader>();
        uint16_t flags = hdr ? hdr->flags : 0;
        // From here on the object is read through this root: the constructor
        // lookups below (globalValue builds lazily), every getProperty and
        // every call may allocate and move it.
        ev::Persistent self(val);

        // The collections are ordinary objects with a real prototype (bronze
        // 24.1.4 and friends), so they are told apart the way Date and Promise
        // are below: by their constructor, not by a heap kind.
        if (isInstanceOf(self, "WeakMap") || isInstanceOf(self, "WeakSet") ||
            isInstanceOf(self, "WeakRef")) {
            ev::throwTypeError("postMessage: weak collections are not cloneable");
            return false;
        }

        if (isInstanceOf(self, "Promise")) {
            ev::throwTypeError("postMessage: Promises are not cloneable");
            return false;
        }
        if (isInstanceOf(self, "Node")) {
            ev::throwTypeError("postMessage: DOM Nodes are not cloneable");
            return false;
        }
#if BRO_WITH_3D
        // A Mesh is TRANSFERABLE and never cloned: its MeshData moves across
        // by pointer (Message::transferredMeshes) and the sender's handle is
        // left empty, the way a transferred ArrayBuffer is detached. It must
        // be in the transfer list — cloning a mesh silently would copy what
        // is often megabytes of geometry, and a sendClone over the network
        // (an empty transfer list) has no realm to receive a pointer.
        if (bromesh::api::isMeshValue(self.get())) {
            if (!isTransferred(self.get(), transfers)) {
                ev::throwTypeError("postMessage: Mesh must be listed in the transferList");
                return false;
            }
            auto data = std::make_unique<bromesh::MeshData>();
            bromesh::api::takeMeshData(self.get(), *data);
            uint32_t idx = static_cast<uint32_t>(out.transferredMeshes.size());
            out.transferredMeshes.push_back(std::move(data));
            w.u8(kTransferMesh);
            w.u32(idx);
            return true;
        }
#endif
        // A native handle (SceneNode, GpuTensor, ...) is a pointer into this
        // realm's engine state. Its own properties are all on the prototype,
        // so the generic path below would clone it as `{}` and the receiver
        // would get an empty object where it expected the resource.
        // (ImageBitmap and Mesh are handles too, but were answered above.)
        if (ev::handleData(self.get())) {
            ev::throwTypeError("postMessage: native objects are not cloneable");
            return false;
        }

        const bool isMap = isInstanceOf(self, "Map");
        if (isMap || isInstanceOf(self, "Set")) {
            Value arrFrom = ev::getProperty(ev::globalValue("Array").value, "from");
            Value collVal = self.get();
            ev::Persistent flat(ev::call(arrFrom, ev::undefined(), std::span<const Value>(&collVal, 1)).value);
            uint32_t len = satCast<uint32_t>(ev::toDouble(ev::getProperty(flat.get(), "length")));
            w.u8(isMap ? kMap : kSet);
            w.u32(len);
            for (uint32_t i = 0; i < len; ++i) {
                ev::Persistent entry(ev::getElement(flat.get(), i));
                if (isMap) {
                    Value k = ev::getElement(entry.get(), 0);
                    if (!writeValue(k, w, st, out, depth + 1)) return false;
                    Value v = ev::getElement(entry.get(), 1);
                    if (!writeValue(v, w, st, out, depth + 1)) return false;
                } else {
                    Value item = entry.get();
                    if (!writeValue(item, w, st, out, depth + 1)) return false;
                }
            }
            return true;
        }

        if (flags == bronze::HeapKind::RegExp) {
            std::string src = ev::toUtf8(ev::getProperty(self.get(), "source"));
            std::string flagsStr = ev::toUtf8(ev::getProperty(self.get(), "flags"));
            w.u8(kRegExp);
            w.u32(static_cast<uint32_t>(src.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(src.data()), src.size());
            w.u32(static_cast<uint32_t>(flagsStr.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(flagsStr.data()), flagsStr.size());
            return true;
        }

        if (flags == bronze::HeapKind::DataView) {
            // The numbers first; the buffer last, its bytes read straight
            // after the getProperty that answered it.
            uint32_t off = satCast<uint32_t>(ev::toDouble(ev::getProperty(self.get(), "byteOffset")));
            uint32_t viewBytes = satCast<uint32_t>(ev::toDouble(ev::getProperty(self.get(), "byteLength")));
            Value buf = ev::getProperty(self.get(), "buffer");
            w.u8(kDataView);
            w.u32(off);
            w.u32(viewBytes);
            return writeValue(buf, w, st, out, depth + 1);
        }

        if (isInstanceOf(self, "Date")) {
            Value getTimeFn = ev::getProperty(self.get(), "getTime");
            double ms = 0;
            if (ev::isFunction(getTimeFn)) {
                Value r = ev::call(getTimeFn, self.get(), {}).value;
                ms = ev::toDouble(r);
            }
            w.u8(kDate);
            w.f64(ms);
            return true;
        }

        if (isInstanceOf(self, "Error")) {
            std::string name = ev::toUtf8(ev::getProperty(self.get(), "name"));
            std::string msg = ev::toUtf8(ev::getProperty(self.get(), "message"));
            std::string stack = ev::toUtf8(ev::getProperty(self.get(), "stack"));
            w.u8(kError);
            w.u32(static_cast<uint32_t>(name.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
            w.u32(static_cast<uint32_t>(msg.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
            w.u32(static_cast<uint32_t>(stack.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(stack.data()), stack.size());
            return true;
        }

        Value isArrFn = ev::getProperty(ev::globalValue("Array").value, "isArray");
        Value arrArg = self.get();
        Value isArrVal = ev::call(isArrFn, ev::undefined(), std::span<const Value>(&arrArg, 1)).value;
        if (ev::toBool(isArrVal)) {
            uint32_t len = satCast<uint32_t>(ev::toDouble(ev::getProperty(self.get(), "length")));
            w.u8(kArray);
            w.u32(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value elem = ev::getElement(self.get(), i);
                if (!writeValue(elem, w, st, out, depth + 1)) return false;
            }
            return true;
        }

        Value entriesFn = ev::getProperty(ev::globalValue("Object").value, "entries");
        Value entriesArg = self.get();
        ev::Persistent entries(ev::call(entriesFn, ev::undefined(), std::span<const Value>(&entriesArg, 1)).value);
        uint32_t numProps = satCast<uint32_t>(ev::toDouble(ev::getProperty(entries.get(), "length")));
        w.u8(kObject);
        w.u32(numProps);
        for (uint32_t i = 0; i < numProps; ++i) {
            ev::Persistent entry(ev::getElement(entries.get(), i));
            std::string key = ev::toUtf8(ev::getElement(entry.get(), 0));
            Value propVal = ev::getElement(entry.get(), 1);
            w.u32(static_cast<uint32_t>(key.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
            if (!writeValue(propVal, w, st, out, depth + 1)) return false;
        }
        return true;
    }

    ev::throwTypeError("postMessage: value is not cloneable");
    return false;
}

static bool readStr(Reader& r, std::string& out) {
    if (!r.ok(4)) return false;
    uint32_t len = r.u32();
    if (!r.ok(len)) return false;
    const uint8_t* p = r.ptr(len);
    out.assign(reinterpret_cast<const char*>(p), len);
    return true;
}

// The reader's side of the memory: each object made so far, numbered in the
// order the writer numbered them, for kObjectRef to name. The writer numbers
// an object before writing what it holds, so the reader claims the number
// when it reads the tag (readValue) and fills it once the object exists — a
// container before reading its children, so a cycle back to it resolves.
using ObjectRoots = std::vector<ev::Persistent>;

static bool makesObject(uint8_t tag) {
    switch (tag) {
    case kArray: case kObject: case kArrayBuffer: case kTransferIndex:
    case kTypedArray: case kTransferImageBitmap: case kDate: case kRegExp:
    case kMap: case kSet: case kError: case kDataView: case kTransferMesh:
        return true;
    default:
        return false;
    }
}

static Value readValue(Reader& r, const Message& msg, ObjectRoots& objs, int depth);

// Reads the ArrayBuffer a view is over: a fresh one or a reference.
static Value readViewBuffer(Reader& r, const Message& msg, ObjectRoots& objs, int depth) {
    if (!r.ok(1)) return ev::throwTypeError("postMessage: truncated view buffer");
    Value ab = readValue(r, msg, objs, depth + 1);
    if (!ev::isArrayBuffer(ab)) return ev::throwTypeError("postMessage: view over a non-buffer");
    return ab;
}

// `slot` is the number readValue claimed for the object this tag makes.
static Value readTagged(uint8_t tag, Reader& r, const Message& msg,
                        ObjectRoots& objs, size_t slot, int depth);

static Value readValue(Reader& r, const Message& msg, ObjectRoots& objs, int depth) {
    if (depth > 64) return ev::throwTypeError("postMessage: object too deeply nested");
    if (!r.ok(1)) return ev::throwTypeError("postMessage: truncated data");

    const uint8_t tag = r.u8();
    if (!makesObject(tag)) return readTagged(tag, r, msg, objs, 0, depth);
    const size_t slot = objs.size();
    objs.emplace_back(ev::undefined());
    Value v = readTagged(tag, r, msg, objs, slot, depth);
    objs[slot].set(v);   // no allocation
    return v;
}

static Value readTagged(uint8_t tag, Reader& r, const Message& msg,
                        ObjectRoots& objs, size_t slot, int depth) {
    switch (tag) {
    case kUndefined: return ev::undefined();
    case kNull:      return ev::null();
    case kTrue:      return ev::fromBool(true);
    case kFalse:     return ev::fromBool(false);
    case kInt32: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated int32");
        return ev::fromDouble(static_cast<int32_t>(r.u32()));
    }
    case kFloat64: {
        if (!r.ok(8)) return ev::throwTypeError("postMessage: truncated float64");
        return ev::fromDouble(r.f64());
    }
    case kString: {
        std::string s;
        if (!readStr(r, s)) return ev::throwTypeError("postMessage: truncated string");
        return ev::fromUtf8(s);
    }
    case kBigInt: {
        std::string s;
        if (!readStr(r, s)) return ev::throwTypeError("postMessage: truncated bigint");
        // The string first and rooted, the constructor read after it: either
        // order leaves one raw across the other's allocation otherwise.
        ev::Persistent strRoot(ev::fromUtf8(s));
        Value ctor = getGlobal("BigInt");
        Value strV = strRoot.get();
        return ev::call(ctor, ev::undefined(), std::span<const Value>(&strV, 1)).value;
    }
    case kArrayBuffer: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated arraybuffer length");
        uint32_t len = r.u32();
        if (!r.ok(len)) return ev::throwTypeError("postMessage: truncated arraybuffer data");
        const uint8_t* p = r.ptr(len);
        return ev::createArrayBuffer(std::span<const uint8_t>(p, len));
    }
    case kTransferIndex: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated transfer index");
        uint32_t idx = r.u32();
        if (idx >= msg.transferredBuffers.size()) return ev::throwTypeError("postMessage: invalid transfer index");
        const auto& buf = msg.transferredBuffers[idx];
        return ev::createArrayBuffer(std::span<const uint8_t>(buf.data(), buf.size()));
    }
    case kObjectRef: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated object reference");
        uint32_t idx = r.u32();
        // A number is claimed before the object exists, so a reference to
        // one still being built (a view's own number, from inside its
        // buffer) would read undefined; no writer produces that.
        if (idx >= objs.size()) return ev::throwTypeError("postMessage: invalid object reference");
        return objs[idx].get();
    }
    case kTransferImageBitmap: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated imagebitmap transfer index");
        uint32_t idx = r.u32();
        if (idx >= msg.transferredImages.size()) return ev::throwTypeError("postMessage: invalid imagebitmap index");
        const auto& simg = msg.transferredImages[idx];
        return wrapHostImageBitmap(simg.pixels.data(), simg.width, simg.height);
    }
    case kTransferMesh: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated mesh transfer index");
        uint32_t idx = r.u32();
#if BRO_WITH_3D
        if (idx >= msg.transferredMeshes.size() || !msg.transferredMeshes[idx]) {
            return ev::throwTypeError("postMessage: invalid mesh transfer index");
        }
        // One-shot: the slot is emptied so a second deserialize of the same
        // message cannot hand out the geometry twice.
        std::unique_ptr<bromesh::MeshData> data = std::move(msg.transferredMeshes[idx]);
        return bromesh::api::makeMeshValue(std::move(*data));
#else
        (void)idx;
        return ev::throwTypeError("postMessage: Mesh transfer needs BRO_WITH_3D");
#endif
    }
    case kTypedArray: {
        if (!r.ok(1 + 4 + 4)) return ev::throwTypeError("postMessage: truncated typed array header");
        uint8_t subtype = r.u8();
        uint32_t offset = r.u32();
        uint32_t viewBytes = r.u32();
        Value ab = readViewBuffer(r, msg, objs,depth);
        uint32_t bpe = 1;
        switch (subtype) {
            case 3: // Int16
            case 4: // Uint16
            case 9: // Float16
                bpe = 2; break;
            case 5: // Int32
            case 6: // Uint32
            case 7: // Float32
                bpe = 4; break;
            case 8: // Float64
            case 10: // BigInt64
            case 11: // BigUint64
                bpe = 8; break;
            default:
                bpe = 1; break;
        }
        auto kind = static_cast<bronze::ElementKind>(subtype);
        return ev::createTypedArrayView(kind, ab, offset, viewBytes / bpe);
    }
    case kDate: {
        if (!r.ok(8)) return ev::throwTypeError("postMessage: truncated date");
        double ms = r.f64();
        Value ctor = getGlobal("Date");
        Value msVal = ev::fromDouble(ms);
        return ev::construct(ctor, std::span<const Value>(&msVal, 1)).value;
    }
    case kRegExp: {
        std::string src, flags;
        if (!readStr(r, src) || !readStr(r, flags)) return ev::throwTypeError("postMessage: truncated regexp");
        ev::Persistent srcV(ev::fromUtf8(src));
        ev::Persistent flagsV(ev::fromUtf8(flags));
        Value ctor = getGlobal("RegExp");
        const Value args[2] = { srcV.get(), flagsV.get() };
        return ev::construct(ctor, std::span<const Value>(args, 2)).value;
    }
    case kMap:
    case kSet: {
        bool isMap = (tag == kMap);
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated collection length");
        uint32_t len = r.u32();
        Value ctor = getGlobal(isMap ? "Map" : "Set");
        ev::Persistent coll(ev::construct(ctor, {}).value);
        objs[slot].set(coll.get());
        ev::Persistent adder(ev::getProperty(coll.get(), isMap ? "set" : "add"));
        for (uint32_t i = 0; i < len; ++i) {
            Value aVal = readValue(r, msg, objs,depth + 1);
            ev::Persistent a(aVal);
            if (isMap) {
                Value bVal = readValue(r, msg, objs,depth + 1);
                ev::Persistent b(bVal);
                const Value args[2] = { a.get(), b.get() };
                auto res = ev::call(adder.get(), coll.get(), std::span<const Value>(args, 2));
                if (!res.thrown && ev::isObject(res.value)) {
                    coll.set(res.value);
                }
            } else {
                Value arg = a.get();
                auto res = ev::call(adder.get(), coll.get(), std::span<const Value>(&arg, 1));
                if (!res.thrown && ev::isObject(res.value)) {
                    coll.set(res.value);
                }
            }
        }
        return coll.get();
    }
    case kError: {
        std::string name, message, stack;
        if (!readStr(r, name) || !readStr(r, message) || !readStr(r, stack))
            return ev::throwTypeError("postMessage: truncated error");
        const char* ctorName = "Error";
        if (name == "TypeError") ctorName = "TypeError";
        else if (name == "RangeError") ctorName = "RangeError";
        else if (name == "ReferenceError") ctorName = "ReferenceError";
        else if (name == "SyntaxError") ctorName = "SyntaxError";

        ev::Persistent msgRoot(ev::fromUtf8(message));
        Value ctor = getGlobal(ctorName);
        Value msgVal = msgRoot.get();
        ev::Persistent err(ev::construct(ctor, std::span<const Value>(&msgVal, 1)).value);
        // Each string is made in its own statement, before the receiver is
        // read: as one call's arguments their order is unspecified.
        ev::Persistent nameV(ev::fromUtf8(name));
        err.set(ev::setProperty(err.get(), "name", nameV.get()));
        ev::Persistent stackV(ev::fromUtf8(stack));
        err.set(ev::setProperty(err.get(), "stack", stackV.get()));
        return err.get();
    }
    case kDataView: {
        if (!r.ok(4 + 4)) return ev::throwTypeError("postMessage: truncated dataview header");
        uint32_t off = r.u32();
        uint32_t viewBytes = r.u32();
        Value abVal = readViewBuffer(r, msg, objs,depth);
        ev::Persistent ab(abVal);
        Value ctor = getGlobal("DataView");
        const Value args[3] = { ab.get(), ev::fromDouble(off), ev::fromDouble(viewBytes) };
        return ev::construct(ctor, std::span<const Value>(args, 3)).value;
    }
    case kArray: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated array length");
        uint32_t len = r.u32();
        Value arrCtor = getGlobal("Array");
        Value lenVal = ev::fromDouble(len);
        ev::Persistent arr(ev::construct(arrCtor, std::span<const Value>(&lenVal, 1)).value);
        objs[slot].set(arr.get());
        for (uint32_t i = 0; i < len; ++i) {
            Value elemVal = readValue(r, msg, objs,depth + 1);
            ev::Persistent elem(elemVal);
            arr.set(ev::setElement(arr.get(), i, elem.get()));
        }
        return arr.get();
    }
    case kObject: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated object prop count");
        uint32_t numProps = r.u32();
        ev::Persistent obj(ev::createObject());
        objs[slot].set(obj.get());
        for (uint32_t i = 0; i < numProps; ++i) {
            std::string key;
            if (!readStr(r, key)) return ev::throwTypeError("postMessage: truncated key");
            Value propValRaw = readValue(r, msg, objs,depth + 1);
            ev::Persistent propVal(propValRaw);
            obj.set(ev::setProperty(obj.get(), key, propVal.get()));
        }
        return obj.get();
    }
    default:
        return ev::throwTypeError("postMessage: unknown tag");
    }
}

} // namespace

bool serializeMessage(Value val, std::span<const Value> transfers, Message& out) {
    // Rooted before anything allocates; the Persistent constructor itself
    // does not touch the JS heap.
    ev::Persistent root(val);
    TransferRoots transferRoots;
    transferRoots.reserve(transfers.size());
    for (Value t : transfers) transferRoots.emplace_back(t);

    out.data.clear();
    out.transferredBuffers.clear();
    out.transferredImages.clear();
#if BRO_WITH_3D
    out.transferredMeshes.clear();
#endif
    Writer w(out.data);
    CloneState st{transferRoots, {}};
    if (!writeValue(root.get(), w, st, out, 0)) return false;
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

Value deserializeMessage(const Message& msg, size_t offset) {
    if (offset > msg.data.size()) {
        return ev::throwTypeError("deserializeMessage: offset out of range");
    }
    Reader r(msg.data.data() + offset, msg.data.size() - offset);
    ObjectRoots objs;
    return readValue(r, msg, objs, 0);
}

} // namespace bro::bronze_host

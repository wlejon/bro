#include "bronze_host/host_worker_msg.h"
#include "bronze_host/gl_internal.h"
#include "abi/bronze_abi.h"
#include "runtime/heap.h"
#if BRO_WITH_3D
#include <bromesh/api.h>
#endif
#include <cstring>
#include <memory>

namespace bro::bronze_host {

namespace {

static bool isInstanceOf(Value val, const char* globalCtorName) {
    if (!ev::isObject(val)) return false;
    auto g = ev::globalValue(globalCtorName);
    if (!g.found || !ev::isObject(g.value)) return false;
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

static bool isTransferred(Value v, std::span<const Value> transfers) {
    for (Value t : transfers) {
        if (t.rawBits() == v.rawBits()) return true;
    }
    return false;
}

static Value getGlobal(std::string_view name) {
    auto g = ev::globalValue(name);
    return g.found ? g.value : ev::undefined();
}

static bool writeValue(Value val, Writer& w, std::span<const Value> transfers,
                       Message& out, int depth) {
    auto& transferBufs = out.transferredBuffers;
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
        int64_t i64 = static_cast<int64_t>(d);
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
        auto info = ev::arrayBufferInfo(val);
        if (isTransferred(val, transfers)) {
            uint32_t idx = static_cast<uint32_t>(transferBufs.size());
            if (info.data && info.byteLength > 0) {
                transferBufs.emplace_back(info.data, info.data + info.byteLength);
            } else {
                transferBufs.emplace_back();
            }
            ev::detachArrayBuffer(val);
            w.u8(kTransferIndex);
            w.u32(idx);
            return true;
        } else {
            w.u8(kArrayBuffer);
            w.u32(info.byteLength);
            if (info.data && info.byteLength > 0) {
                w.bytes(info.data, info.byteLength);
            }
            return true;
        }
    }

    if (ev::isTypedArray(val)) {
        auto info = ev::typedArrayInfo(val);
        uint8_t subtype = static_cast<uint8_t>(info.elementKind);
        Value bufVal = ev::typedArrayBuffer(val);
        uint32_t offset = ev::typedArrayByteOffset(val);
        auto bufInfo = ev::arrayBufferInfo(bufVal);

        w.u8(kTypedArray);
        w.u8(subtype);
        w.u32(offset);
        w.u32(info.byteLength);
        w.u32(bufInfo.byteLength);
        if (bufInfo.data && bufInfo.byteLength > 0) {
            w.bytes(bufInfo.data, bufInfo.byteLength);
        }
        return true;
    }

    if (ev::isFunction(val)) {
        ev::throwTypeError("postMessage: functions are not cloneable");
        return false;
    }

    if (ev::isObject(val)) {
        const auto* hdr = val.asObject<bronze::HeapObjectHeader>();
        uint16_t flags = hdr ? hdr->flags : 0;

        // The collections are ordinary objects with a real prototype (bronze
        // 24.1.4 and friends), so they are told apart the way Date and Promise
        // are below: by their constructor, not by a heap kind.
        if (isInstanceOf(val, "WeakMap") || isInstanceOf(val, "WeakSet") ||
            isInstanceOf(val, "WeakRef")) {
            ev::throwTypeError("postMessage: weak collections are not cloneable");
            return false;
        }

        if (isInstanceOf(val, "Promise")) {
            ev::throwTypeError("postMessage: Promises are not cloneable");
            return false;
        }
        if (isInstanceOf(val, "Node")) {
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
        if (bromesh::api::isMeshValue(val)) {
            if (!isTransferred(val, transfers)) {
                ev::throwTypeError("postMessage: Mesh must be listed in the transferList");
                return false;
            }
            auto data = std::make_unique<bromesh::MeshData>();
            bromesh::api::takeMeshData(val, *data);
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
        if (ev::handleData(val)) {
            ev::throwTypeError("postMessage: native objects are not cloneable");
            return false;
        }

        const bool isMap = isInstanceOf(val, "Map");
        if (isMap || isInstanceOf(val, "Set")) {
            ev::Persistent self(val);
            Value arrFrom = ev::getProperty(ev::globalValue("Array").value, "from");
            Value collVal = self.get();
            ev::Persistent flat(ev::call(arrFrom, ev::undefined(), std::span<const Value>(&collVal, 1)).value);
            uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(flat.get(), "length")));
            w.u8(isMap ? kMap : kSet);
            w.u32(len);
            for (uint32_t i = 0; i < len; ++i) {
                ev::Persistent entry(ev::getElement(flat.get(), i));
                if (isMap) {
                    Value k = ev::getElement(entry.get(), 0);
                    if (!writeValue(k, w, transfers, out, depth + 1)) return false;
                    Value v = ev::getElement(entry.get(), 1);
                    if (!writeValue(v, w, transfers, out, depth + 1)) return false;
                } else {
                    Value item = entry.get();
                    if (!writeValue(item, w, transfers, out, depth + 1)) return false;
                }
            }
            return true;
        }

        if (flags == bronze::HeapKind::RegExp) {
            std::string src = ev::toUtf8(ev::getProperty(val, "source"));
            std::string flagsStr = ev::toUtf8(ev::getProperty(val, "flags"));
            w.u8(kRegExp);
            w.u32(static_cast<uint32_t>(src.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(src.data()), src.size());
            w.u32(static_cast<uint32_t>(flagsStr.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(flagsStr.data()), flagsStr.size());
            return true;
        }

        if (flags == bronze::HeapKind::DataView) {
            Value buf = ev::getProperty(val, "buffer");
            uint32_t off = static_cast<uint32_t>(ev::toDouble(ev::getProperty(val, "byteOffset")));
            uint32_t viewBytes = static_cast<uint32_t>(ev::toDouble(ev::getProperty(val, "byteLength")));
            auto bInfo = ev::arrayBufferInfo(buf);
            w.u8(kDataView);
            w.u32(off);
            w.u32(viewBytes);
            w.u32(bInfo.byteLength);
            if (bInfo.data && bInfo.byteLength > 0) {
                w.bytes(bInfo.data, bInfo.byteLength);
            }
            return true;
        }

        if (isInstanceOf(val, "Date")) {
            Value getTimeFn = ev::getProperty(val, "getTime");
            double ms = 0;
            if (ev::isFunction(getTimeFn)) {
                Value r = ev::call(getTimeFn, val, {}).value;
                ms = ev::toDouble(r);
            }
            w.u8(kDate);
            w.f64(ms);
            return true;
        }

        if (isInstanceOf(val, "Error")) {
            std::string name = ev::toUtf8(ev::getProperty(val, "name"));
            std::string msg = ev::toUtf8(ev::getProperty(val, "message"));
            std::string stack = ev::toUtf8(ev::getProperty(val, "stack"));
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
        Value isArrVal = ev::call(isArrFn, ev::undefined(), std::span<const Value>(&val, 1)).value;
        if (ev::toBool(isArrVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(val, "length")));
            w.u8(kArray);
            w.u32(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value elem = ev::getElement(val, i);
                if (!writeValue(elem, w, transfers, out, depth + 1)) return false;
            }
            return true;
        }

        Value entriesFn = ev::getProperty(ev::globalValue("Object").value, "entries");
        ev::Persistent entries(ev::call(entriesFn, ev::undefined(), std::span<const Value>(&val, 1)).value);
        uint32_t numProps = static_cast<uint32_t>(ev::toDouble(ev::getProperty(entries.get(), "length")));
        w.u8(kObject);
        w.u32(numProps);
        for (uint32_t i = 0; i < numProps; ++i) {
            ev::Persistent entry(ev::getElement(entries.get(), i));
            std::string key = ev::toUtf8(ev::getElement(entry.get(), 0));
            Value propVal = ev::getElement(entry.get(), 1);
            w.u32(static_cast<uint32_t>(key.size()));
            w.bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
            if (!writeValue(propVal, w, transfers, out, depth + 1)) return false;
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

static Value readValue(Reader& r, const Message& msg, int depth) {
    if (depth > 64) return ev::throwTypeError("postMessage: object too deeply nested");
    if (!r.ok(1)) return ev::throwTypeError("postMessage: truncated data");

    uint8_t tag = r.u8();
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
        Value ctor = getGlobal("BigInt");
        Value strV = ev::fromUtf8(s);
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
        if (!r.ok(1 + 4 + 4 + 4)) return ev::throwTypeError("postMessage: truncated typed array header");
        uint8_t subtype = r.u8();
        uint32_t offset = r.u32();
        uint32_t viewBytes = r.u32();
        uint32_t bufBytes = r.u32();
        if (!r.ok(bufBytes)) return ev::throwTypeError("postMessage: truncated typed array data");
        const uint8_t* bufData = r.ptr(bufBytes);
        Value ab = ev::createArrayBuffer(std::span<const uint8_t>(bufData, bufBytes));
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
        Value ctor = getGlobal("RegExp");
        const Value args[2] = { ev::fromUtf8(src), ev::fromUtf8(flags) };
        return ev::construct(ctor, std::span<const Value>(args, 2)).value;
    }
    case kMap:
    case kSet: {
        bool isMap = (tag == kMap);
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated collection length");
        uint32_t len = r.u32();
        Value ctor = getGlobal(isMap ? "Map" : "Set");
        ev::Persistent coll(ev::construct(ctor, {}).value);
        ev::Persistent adder(ev::getProperty(coll.get(), isMap ? "set" : "add"));
        for (uint32_t i = 0; i < len; ++i) {
            Value aVal = readValue(r, msg, depth + 1);
            if (bronze_exception_pending()) return aVal;
            ev::Persistent a(aVal);
            if (isMap) {
                Value bVal = readValue(r, msg, depth + 1);
                if (bronze_exception_pending()) return bVal;
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

        Value ctor = getGlobal(ctorName);
        Value msgVal = ev::fromUtf8(message);
        ev::Persistent err(ev::construct(ctor, std::span<const Value>(&msgVal, 1)).value);
        err.set(ev::setProperty(err.get(), "name", ev::fromUtf8(name)));
        err.set(ev::setProperty(err.get(), "stack", ev::fromUtf8(stack)));
        return err.get();
    }
    case kDataView: {
        if (!r.ok(4 + 4 + 4)) return ev::throwTypeError("postMessage: truncated dataview header");
        uint32_t off = r.u32();
        uint32_t viewBytes = r.u32();
        uint32_t bufBytes = r.u32();
        if (!r.ok(bufBytes)) return ev::throwTypeError("postMessage: truncated dataview data");
        const uint8_t* p = r.ptr(bufBytes);
        ev::Persistent ab(ev::createArrayBuffer(std::span<const uint8_t>(p, bufBytes)));
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
        for (uint32_t i = 0; i < len; ++i) {
            Value elemVal = readValue(r, msg, depth + 1);
            if (bronze_exception_pending()) return elemVal;
            ev::Persistent elem(elemVal);
            arr.set(ev::setElement(arr.get(), i, elem.get()));
        }
        return arr.get();
    }
    case kObject: {
        if (!r.ok(4)) return ev::throwTypeError("postMessage: truncated object prop count");
        uint32_t numProps = r.u32();
        ev::Persistent obj(ev::createObject());
        for (uint32_t i = 0; i < numProps; ++i) {
            std::string key;
            if (!readStr(r, key)) return ev::throwTypeError("postMessage: truncated key");
            Value propValRaw = readValue(r, msg, depth + 1);
            if (bronze_exception_pending()) return propValRaw;
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
    out.data.clear();
    out.transferredBuffers.clear();
    out.transferredImages.clear();
#if BRO_WITH_3D
    out.transferredMeshes.clear();
#endif
    Writer w(out.data);
    return writeValue(val, w, transfers, out, 0);
}

Value deserializeMessage(const Message& msg, size_t offset) {
    if (offset > msg.data.size()) {
        return ev::throwTypeError("deserializeMessage: offset out of range");
    }
    Reader r(msg.data.data() + offset, msg.data.size() - offset);
    return readValue(r, msg, 0);
}

} // namespace bro::bronze_host

#include "js/message_serializer.h"
#if BRO_WITH_3D  // Mesh transfer needs bromesh (3D-only)
#include "js/mesh_bindings.h"
#include <bromesh/mesh_data.h>
#endif
#include "js/imagebitmap_bindings.h"
#include "util/log.h"
#include <include/core/SkImage.h>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Binary format tags
// ---------------------------------------------------------------------------
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
    kArrayBuffer     = 0x09,  // copied
    kTransferIndex   = 0x0A,  // index into transferredBuffers
    kTypedArray      = 0x0B,  // TypedArray: subtype + byte offset + length + ArrayBuffer
    kTransferMesh    = 0x0C,  // index into transferredObjects (zero-copy Mesh)
    kBigInt          = 0x0D,  // arbitrary-precision: decimal string representation
    kTransferImageBitmap = 0x0E,  // index into transferredObjects (ImageBitmap)
};

#if BRO_WITH_3D
// Deleter for MeshData* stored in TransferredObject (type=kMesh).
static void deleteMeshData(void* p) {
    delete static_cast<bromesh::MeshData*>(p);
}
#endif  // BRO_WITH_3D

// Deleter for SkImage* stored in TransferredObject (type=kImageBitmap).
// The slot owns exactly one ref; releasing it drops that ref.
static void deleteSkImage(void* p) {
    static_cast<SkImage*>(p)->unref();
}

// ---------------------------------------------------------------------------
// Writer / Reader helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------
static bool writeValue(JSContext* ctx, JSValue val, Writer& w,
                       const JSValue* transfers, size_t numTransfers,
                       std::vector<std::vector<uint8_t>>& transferBufs,
                       std::vector<TransferredObject>& transferObjs,
                       int depth, bool forNetwork)
{
    if (depth > 64) {
        JS_ThrowTypeError(ctx, "postMessage: object too deeply nested");
        return false;
    }

    if (JS_IsUndefined(val)) { w.u8(kUndefined); return true; }
    if (JS_IsNull(val))      { w.u8(kNull);      return true; }

    if (JS_IsBool(val)) {
        w.u8(JS_ToBool(ctx, val) ? kTrue : kFalse);
        return true;
    }

    // Number — int or float
    int tag = JS_VALUE_GET_TAG(val);
    if (tag == JS_TAG_INT) {
        int32_t v;
        JS_ToInt32(ctx, &v, val);
        w.u8(kInt32);
        w.u32(static_cast<uint32_t>(v));
        return true;
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        double v;
        JS_ToFloat64(ctx, &v, val);
        w.u8(kFloat64);
        w.f64(v);
        return true;
    }

    // BigInt — arbitrary precision, encoded as its decimal string. We could
    // fast-path int64-fit values but the saving is marginal and a single
    // encoding keeps deserialization branch-free.
    if (JS_IsBigInt(val)) {
        JSValue strVal = JS_ToString(ctx, val);
        if (JS_IsException(strVal)) return false;
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, strVal);
        JS_FreeValue(ctx, strVal);
        if (!s) return false;
        w.u8(kBigInt);
        w.u32(static_cast<uint32_t>(len));
        w.bytes(reinterpret_cast<const uint8_t*>(s), len);
        JS_FreeCString(ctx, s);
        return true;
    }

    // String
    if (JS_IsString(val)) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return false;
        w.u8(kString);
        w.u32(static_cast<uint32_t>(len));
        w.bytes(reinterpret_cast<const uint8_t*>(s), len);
        JS_FreeCString(ctx, s);
        return true;
    }

    // Mesh — C++-backed opaque object. Only transferable, never structured-cloned:
    // the underlying MeshData lives in a unique_ptr inside the MeshWrapper, and
    // we move it across threads by pointer (no byte copy). The source Mesh is
    // left neutered after transfer.
#if BRO_WITH_3D
    {
        JSClassID meshClassId = MeshBindings::classId();
        if (meshClassId != 0 && JS_GetOpaque(val, meshClassId) != nullptr) {
            if (forNetwork) {
                JS_ThrowTypeError(ctx, "sendClone: Mesh cannot be sent over the network "
                                       "(it transfers by pointer; export bytes instead)");
                return false;
            }
            // Must be in the transfer list — otherwise structured-cloning a Mesh
            // is not supported.
            for (size_t i = 0; i < numTransfers; i++) {
                if (JS_VALUE_GET_PTR(val) == JS_VALUE_GET_PTR(transfers[i])) {
                    auto data = MeshBindings::takeMeshData(ctx, val);
                    if (!data) {
                        JS_ThrowTypeError(ctx, "postMessage: Mesh is already neutered");
                        return false;
                    }
                    // Hand off raw pointer to the Message. release() so the
                    // unique_ptr doesn't delete on scope exit.
                    uint32_t idx = static_cast<uint32_t>(transferObjs.size());
                    transferObjs.emplace_back(TransferredObject::kMesh,
                                              static_cast<void*>(data.release()),
                                              &deleteMeshData);
                    w.u8(kTransferMesh);
                    w.u32(idx);
                    return true;
                }
            }
            JS_ThrowTypeError(ctx, "postMessage: Mesh must be listed in the transferList");
            return false;
        }
    }
#endif  // BRO_WITH_3D

    // ImageBitmap — an immutable raster SkImage. Listed in the transfer list:
    // moved zero-copy, source neutered. Not listed: structured-cloned — but the
    // SkImage is immutable, so we ref-share it rather than copy the pixels
    // (observationally identical to a deep copy, since it can never mutate).
    {
        JSClassID ibClassId = ImageBitmapBindings::classId();
        if (ibClassId != 0 && JS_GetOpaque(val, ibClassId) != nullptr) {
            if (forNetwork) {
                JS_ThrowTypeError(ctx, "sendClone: ImageBitmap cannot be sent over the "
                                       "network (it ref-shares in-process pixels; send "
                                       "raw pixel bytes instead)");
                return false;
            }
            bool transfer = false;
            for (size_t i = 0; i < numTransfers; i++) {
                if (JS_VALUE_GET_PTR(val) == JS_VALUE_GET_PTR(transfers[i])) {
                    transfer = true;
                    break;
                }
            }
            sk_sp<SkImage> img = transfer
                ? ImageBitmapBindings::takeImage(val)
                : ImageBitmapBindings::getImage(val);
            if (!img) {
                JS_ThrowTypeError(ctx, "postMessage: ImageBitmap is closed");
                return false;
            }
            uint32_t idx = static_cast<uint32_t>(transferObjs.size());
            transferObjs.emplace_back(TransferredObject::kImageBitmap,
                                      static_cast<void*>(img.release()),
                                      &deleteSkImage);
            w.u8(kTransferImageBitmap);
            w.u32(idx);
            return true;
        }
    }

    // ArrayBuffer — check transfer list first
    if (JS_IsArrayBuffer(val)) {
        for (size_t i = 0; i < numTransfers; i++) {
            if (JS_VALUE_GET_PTR(val) == JS_VALUE_GET_PTR(transfers[i])) {
                // Transfer: copy data out, detach source
                size_t abSize;
                uint8_t* abData = JS_GetArrayBuffer(ctx, &abSize, val);
                w.u8(kTransferIndex);
                w.u32(static_cast<uint32_t>(transferBufs.size()));
                if (abData && abSize > 0)
                    transferBufs.emplace_back(abData, abData + abSize);
                else
                    transferBufs.emplace_back();
                JS_DetachArrayBuffer(ctx, val);
                return true;
            }
        }
        // Clone: copy data
        size_t abSize;
        uint8_t* abData = JS_GetArrayBuffer(ctx, &abSize, val);
        w.u8(kArrayBuffer);
        w.u32(static_cast<uint32_t>(abSize));
        if (abData && abSize > 0)
            w.bytes(abData, abSize);
        return true;
    }

    // TypedArray (Float32Array, Uint32Array, Int32Array, Uint8Array, etc.)
    {
        size_t abOffset, abSize;
        size_t abBufLen;
        JSValue abBuf = JS_GetTypedArrayBuffer(ctx, val, &abOffset, &abSize, nullptr);
        if (!JS_IsException(abBuf)) {
            // It's a TypedArray — get its element type.
            //
            // Ask QuickJS for the class rather than reading constructor.name.
            // The name is wrong in two ways that both used to land on the
            // Float32 default and reinterpret the bytes silently: BigInt64Array
            // / BigUint64Array / Float16Array were simply absent from the table,
            // and a subclass (`class Px extends Uint8Array {}`) reports its own
            // name, not the base class's. An unknown class must throw, never
            // fall back to a guess.
            int taType = JS_GetTypedArrayType(val);
            uint8_t subtype;
            switch (taType) {
                case JS_TYPED_ARRAY_INT8:       subtype = 0;  break;
                case JS_TYPED_ARRAY_UINT8:      subtype = 1;  break;
                case JS_TYPED_ARRAY_UINT8C:     subtype = 2;  break;
                case JS_TYPED_ARRAY_INT16:      subtype = 3;  break;
                case JS_TYPED_ARRAY_UINT16:     subtype = 4;  break;
                case JS_TYPED_ARRAY_INT32:      subtype = 5;  break;
                case JS_TYPED_ARRAY_UINT32:     subtype = 6;  break;
                case JS_TYPED_ARRAY_FLOAT32:    subtype = 7;  break;
                case JS_TYPED_ARRAY_FLOAT64:    subtype = 8;  break;
                case JS_TYPED_ARRAY_BIG_INT64:  subtype = 9;  break;
                case JS_TYPED_ARRAY_BIG_UINT64: subtype = 10; break;
                case JS_TYPED_ARRAY_FLOAT16:    subtype = 11; break;
                default:
                    JS_FreeValue(ctx, abBuf);
                    JS_ThrowTypeError(ctx,
                        "postMessage: unsupported TypedArray element type %d", taType);
                    return false;
            }

            // Get the raw bytes from the underlying ArrayBuffer
            size_t bufLen;
            uint8_t* bufData = JS_GetArrayBuffer(ctx, &bufLen, abBuf);

            w.u8(kTypedArray);
            w.u8(subtype);
            w.u32(static_cast<uint32_t>(abOffset));
            w.u32(static_cast<uint32_t>(abSize));   // byte length of the view
            w.u32(static_cast<uint32_t>(bufLen));    // byte length of underlying buffer
            if (bufData && bufLen > 0)
                w.bytes(bufData, bufLen);

            JS_FreeValue(ctx, abBuf);
            return true;
        }
        // Not a TypedArray — JS_GetTypedArrayBuffer returned exception, clear it
        // (QuickJS sets exception on non-TypedArray, we just ignore it)
    }

    // Array
    if (JS_IsArray(val)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, val, "length");
        uint32_t len;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        w.u8(kArray);
        w.u32(len);
        for (uint32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, val, i);
            bool ok = writeValue(ctx, elem, w, transfers, numTransfers, transferBufs, transferObjs, depth + 1, forNetwork);
            JS_FreeValue(ctx, elem);
            if (!ok) return false;
        }
        return true;
    }

    // Functions are objects to JS_IsObject but are not structured-clonable —
    // without this check they'd silently serialize as {} via the plain-object
    // branch below (real structured clone throws DataCloneError).
    if (JS_IsFunction(ctx, val)) {
        JS_ThrowTypeError(ctx, "postMessage: function is not cloneable");
        return false;
    }

    // Plain object
    if (JS_IsObject(val)) {
        JSPropertyEnum* props = nullptr;
        uint32_t numProps = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &numProps, val,
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return false;

        w.u8(kObject);
        w.u32(numProps);

        bool ok = true;
        for (uint32_t i = 0; i < numProps; i++) {
            // Key
            const char* key = JS_AtomToCString(ctx, props[i].atom);
            if (!key) { ok = false; break; }
            uint32_t keyLen = static_cast<uint32_t>(strlen(key));
            w.u32(keyLen);
            w.bytes(reinterpret_cast<const uint8_t*>(key), keyLen);
            JS_FreeCString(ctx, key);

            // Value
            JSValue propVal = JS_GetProperty(ctx, val, props[i].atom);
            ok = writeValue(ctx, propVal, w, transfers, numTransfers, transferBufs, transferObjs, depth + 1, forNetwork);
            JS_FreeValue(ctx, propVal);
            if (!ok) break;
        }

        for (uint32_t i = 0; i < numProps; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        return ok;
    }

    // Functions, symbols, etc. — not cloneable
    JS_ThrowTypeError(ctx, "postMessage: value is not cloneable");
    return false;
}

bool serializeMessage(JSContext* ctx, JSValue value, JSValue transferList, Message& out,
                      bool forNetwork)
{
    // A network payload cannot carry transfers: transferred ArrayBuffers and
    // pointer-transferred C++ objects only exist as in-process side tables.
    if (forNetwork && !JS_IsUndefined(transferList) && !JS_IsNull(transferList)) {
        JS_ThrowTypeError(ctx, "sendClone: transfer lists are not supported over the network");
        return false;
    }
    // Collect transfer list. Allowed entries: ArrayBuffer (data copy today)
    // or Mesh (true zero-copy transfer of the underlying bromesh::MeshData).
    std::vector<JSValue> transfers;
#if BRO_WITH_3D
    JSClassID meshClassId = MeshBindings::classId();
#else
    JSClassID meshClassId = 0;  // no Mesh transfer without 3D
#endif
    JSClassID ibClassId   = ImageBitmapBindings::classId();
    if (!JS_IsUndefined(transferList) && JS_IsArray(transferList)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, transferList, "length");
        uint32_t len;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        transfers.resize(len);
        for (uint32_t i = 0; i < len; i++) {
            transfers[i] = JS_GetPropertyUint32(ctx, transferList, i);
            bool isAB   = JS_IsArrayBuffer(transfers[i]);
            bool isMesh = meshClassId != 0 &&
                          JS_GetOpaque(transfers[i], meshClassId) != nullptr;
            bool isImageBitmap = ibClassId != 0 &&
                          JS_GetOpaque(transfers[i], ibClassId) != nullptr;
            if (!isAB && !isMesh && !isImageBitmap) {
                for (uint32_t j = 0; j <= i; j++)
                    JS_FreeValue(ctx, transfers[j]);
                JS_ThrowTypeError(ctx, "postMessage: transfer list must contain ArrayBuffers, Mesh, or ImageBitmap objects");
                return false;
            }
        }
    }

    out.data.clear();
    out.transferredBuffers.clear();
    out.transferredObjects.clear();

    Writer w(out.data);
    bool ok = writeValue(ctx, value, w,
                         transfers.data(), transfers.size(),
                         out.transferredBuffers, out.transferredObjects, 0, forNetwork);

    for (auto& t : transfers)
        JS_FreeValue(ctx, t);
    return ok;
}

// ---------------------------------------------------------------------------
// Deserialize
// ---------------------------------------------------------------------------
static JSValue readValue(JSContext* ctx, Reader& r, Message& msg, int depth)
{
    // Mirror of the writer's depth>64 limit. The writer never produces deeper
    // nesting, so anything past it is a malformed (or hostile) payload — and
    // without this check a crafted stream of nested kArray tags would recurse
    // the C stack to death long before any byte bound tripped.
    if (depth > 64)
        return JS_ThrowTypeError(ctx, "postMessage: object too deeply nested");

    if (!r.ok(1))
        return JS_ThrowTypeError(ctx, "postMessage: truncated data");

    uint8_t tag = r.u8();

    switch (tag) {
    case kUndefined: return JS_UNDEFINED;
    case kNull:      return JS_NULL;
    case kTrue:      return JS_TRUE;
    case kFalse:     return JS_FALSE;

    case kInt32: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated int32");
        int32_t v = static_cast<int32_t>(r.u32());
        return JS_NewInt32(ctx, v);
    }
    case kFloat64: {
        if (!r.ok(8)) return JS_ThrowTypeError(ctx, "postMessage: truncated float64");
        return JS_NewFloat64(ctx, r.f64());
    }
    case kString: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated string length");
        uint32_t len = r.u32();
        if (!r.ok(len)) return JS_ThrowTypeError(ctx, "postMessage: truncated string data");
        const uint8_t* s = r.ptr(len);
        return JS_NewStringLen(ctx, reinterpret_cast<const char*>(s), len);
    }
    case kBigInt: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated bigint length");
        uint32_t len = r.u32();
        if (!r.ok(len)) return JS_ThrowTypeError(ctx, "postMessage: truncated bigint data");
        const uint8_t* s = r.ptr(len);
        // Construct via the global BigInt(string) constructor — handles
        // arbitrary precision without dipping into QuickJS internals.
        JSValue strVal = JS_NewStringLen(ctx, reinterpret_cast<const char*>(s), len);
        if (JS_IsException(strVal)) return strVal;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, "BigInt");
        JS_FreeValue(ctx, global);
        if (JS_IsException(ctor) || !JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            JS_FreeValue(ctx, strVal);
            return JS_ThrowTypeError(ctx, "postMessage: BigInt global not available");
        }
        JSValue result = JS_Call(ctx, ctor, JS_UNDEFINED, 1, &strVal);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, strVal);
        return result;
    }
    case kArrayBuffer: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated arraybuffer length");
        uint32_t len = r.u32();
        if (!r.ok(len)) return JS_ThrowTypeError(ctx, "postMessage: truncated arraybuffer data");
        const uint8_t* p = r.ptr(len);
        return JS_NewArrayBufferCopy(ctx, p, len);
    }
    case kTransferIndex: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated transfer index");
        uint32_t idx = r.u32();
        if (idx >= msg.transferredBuffers.size())
            return JS_ThrowTypeError(ctx, "postMessage: invalid transfer index");
        const auto& buf = msg.transferredBuffers[idx];
        if (buf.empty())
            return JS_NewArrayBufferCopy(ctx, nullptr, 0);
        // Create ArrayBuffer owning a copy of the transferred data
        return JS_NewArrayBufferCopy(ctx, buf.data(), buf.size());
    }
#if BRO_WITH_3D
    case kTransferMesh: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated mesh transfer index");
        uint32_t idx = r.u32();
        if (idx >= msg.transferredObjects.size())
            return JS_ThrowTypeError(ctx, "postMessage: invalid mesh transfer index");
        auto& obj = msg.transferredObjects[idx];
        if (obj.type != TransferredObject::kMesh || !obj.ptr)
            return JS_ThrowTypeError(ctx, "postMessage: mesh transfer slot already consumed");
        // Take ownership: release the pointer from the TransferredObject and
        // hand it to a unique_ptr that the new JS Mesh will own on this thread.
        auto* raw = static_cast<bromesh::MeshData*>(obj.release());
        return MeshBindings::wrapMeshData(ctx, std::unique_ptr<bromesh::MeshData>(raw));
    }
#endif  // BRO_WITH_3D
    case kTransferImageBitmap: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated imagebitmap transfer index");
        uint32_t idx = r.u32();
        if (idx >= msg.transferredObjects.size())
            return JS_ThrowTypeError(ctx, "postMessage: invalid imagebitmap transfer index");
        auto& obj = msg.transferredObjects[idx];
        if (obj.type != TransferredObject::kImageBitmap || !obj.ptr)
            return JS_ThrowTypeError(ctx, "postMessage: imagebitmap transfer slot already consumed");
        // Adopt the single ref carried by the slot (sk_sp(T*) does not add one).
        auto* raw = static_cast<SkImage*>(obj.release());
        return ImageBitmapBindings::wrap(ctx, sk_sp<SkImage>(raw));
    }
    case kTypedArray: {
        if (!r.ok(1 + 4 + 4 + 4))
            return JS_ThrowTypeError(ctx, "postMessage: truncated typed array header");
        uint8_t subtype = r.u8();
        uint32_t offset = r.u32();
        uint32_t viewBytes = r.u32();
        uint32_t bufBytes = r.u32();
        if (!r.ok(bufBytes))
            return JS_ThrowTypeError(ctx, "postMessage: truncated typed array data");
        const uint8_t* bufData = r.ptr(bufBytes);

        // Create ArrayBuffer with the data
        JSValue ab = JS_NewArrayBufferCopy(ctx, bufData, bufBytes);
        if (JS_IsException(ab)) return ab;

        // Map subtype back to a constructor name. An unrecognized subtype is a
        // corrupt or newer-than-us stream: say so rather than handing back a
        // Float32Array over bytes that were never floats.
        const char* ctorName;
        int bytesPerElement;
        switch (subtype) {
            case 0:  ctorName = "Int8Array";          bytesPerElement = 1; break;
            case 1:  ctorName = "Uint8Array";         bytesPerElement = 1; break;
            case 2:  ctorName = "Uint8ClampedArray";  bytesPerElement = 1; break;
            case 3:  ctorName = "Int16Array";         bytesPerElement = 2; break;
            case 4:  ctorName = "Uint16Array";        bytesPerElement = 2; break;
            case 5:  ctorName = "Int32Array";         bytesPerElement = 4; break;
            case 6:  ctorName = "Uint32Array";        bytesPerElement = 4; break;
            case 7:  ctorName = "Float32Array";       bytesPerElement = 4; break;
            case 8:  ctorName = "Float64Array";       bytesPerElement = 8; break;
            case 9:  ctorName = "BigInt64Array";      bytesPerElement = 8; break;
            case 10: ctorName = "BigUint64Array";     bytesPerElement = 8; break;
            case 11: ctorName = "Float16Array";       bytesPerElement = 2; break;
            default:
                JS_FreeValue(ctx, ab);
                return JS_ThrowTypeError(ctx,
                    "postMessage: unknown TypedArray subtype %u", subtype);
        }

        // Create TypedArray: new Ctor(arrayBuffer, byteOffset, length)
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, ctorName);
        JS_FreeValue(ctx, global);

        if (JS_IsException(ctor) || JS_IsUndefined(ctor)) {
            JS_FreeValue(ctx, ctor);
            JS_FreeValue(ctx, ab);
            return JS_ThrowTypeError(ctx, "postMessage: TypedArray constructor %s not found", ctorName);
        }

        uint32_t elemCount = viewBytes / bytesPerElement;
        JSValue args[3] = {
            ab,
            JS_NewUint32(ctx, offset),
            JS_NewUint32(ctx, elemCount)
        };
        JSValue result = JS_CallConstructor(ctx, ctor, 3, args);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, ab);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
        return result;
    }
    case kArray: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated array length");
        uint32_t len = r.u32();
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) return arr;
        for (uint32_t i = 0; i < len; i++) {
            JSValue elem = readValue(ctx, r, msg, depth + 1);
            if (JS_IsException(elem)) {
                JS_FreeValue(ctx, arr);
                return elem;
            }
            JS_SetPropertyUint32(ctx, arr, i, elem);  // takes ownership
        }
        return arr;
    }
    case kObject: {
        if (!r.ok(4)) return JS_ThrowTypeError(ctx, "postMessage: truncated object prop count");
        uint32_t numProps = r.u32();
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) return obj;
        for (uint32_t i = 0; i < numProps; i++) {
            // Key
            if (!r.ok(4)) { JS_FreeValue(ctx, obj); return JS_ThrowTypeError(ctx, "postMessage: truncated key length"); }
            uint32_t keyLen = r.u32();
            if (!r.ok(keyLen)) { JS_FreeValue(ctx, obj); return JS_ThrowTypeError(ctx, "postMessage: truncated key data"); }
            const uint8_t* keyData = r.ptr(keyLen);
            std::string key(reinterpret_cast<const char*>(keyData), keyLen);

            // Value
            JSValue propVal = readValue(ctx, r, msg, depth + 1);
            if (JS_IsException(propVal)) {
                JS_FreeValue(ctx, obj);
                return propVal;
            }
            JS_SetPropertyStr(ctx, obj, key.c_str(), propVal);  // takes ownership
        }
        return obj;
    }
    default:
        return JS_ThrowTypeError(ctx, "postMessage: unknown tag %d", tag);
    }
}

JSValue deserializeMessage(JSContext* ctx, Message& msg, size_t offset)
{
    if (offset > msg.data.size())
        return JS_ThrowTypeError(ctx, "postMessage: truncated data");
    Reader r(msg.data.data() + offset, msg.data.size() - offset);
    return readValue(ctx, r, msg, 0);
}

}  // namespace bro::js

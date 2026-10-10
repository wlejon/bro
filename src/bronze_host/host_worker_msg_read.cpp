// The structured-clone READER (HTML StructuredDeserialize) for the format in
// host_worker_msg_format.h. Every count and length is checked against the
// bytes left before it sizes anything: bro.net hands this bytes off the wire.
//
// Objects are built with bronze's clone primitives (embed/embed_clone.h): a
// layout's keys are interned once per message and each position keeps a
// define cache, so the ten-thousandth object of a layout takes the same
// shape transitions the first one recorded. Out-of-band ArrayBuffer bytes
// (Message::buffers) are adopted as external buffers, never copied.

#include "bronze_host/host_worker_msg.h"
#include "bronze_host/host_worker_msg_format.h"
#include "bronze_host/host_builder.h"
#include "abi/bronze_abi.h"
#include "embed/embed_clone.h"
#include "runtime/heap.h"
#if BRO_WITH_3D
#include <bromesh/api.h>
#endif
#include <array>
#include <cstdlib>
#include <deque>
#include <string>

namespace bro::bronze_host {

using namespace clonefmt;
namespace clone = bronze::embed::clone;

namespace {

Value getGlobal(std::string_view name) {
    auto g = ev::globalValue(name);
    return g.found ? g.value : ev::undefined();
}

void freeBlock(void*, uint8_t* bytes) { std::free(bytes); }

bool makesObject(uint8_t tag) {
    switch (tag) {
    case kArray: case kObject: case kObjectLayout: case kArrayBuffer: case kBufferBlock:
    case kTypedArray: case kTransferImageBitmap: case kDate: case kRegExp:
    case kMap: case kSet: case kError: case kDataView: case kTransferMesh:
        return true;
    default:
        return false;
    }
}

uint32_t bytesPerElement(uint8_t subtype) {
    switch (subtype) {
    case 3: case 4: case 9: return 2;            // Int16, Uint16, Float16
    case 5: case 6: case 7: return 4;            // Int32, Uint32, Float32
    case 8: case 10: case 11: return 8;          // Float64, BigInt64, BigUint64
    default: return 1;
    }
}

class ReaderState {
public:
    ReaderState(const Message& msg, size_t offset)
        : r_(msg.data.data() + offset, msg.data.size() - offset), msg_(msg) {}

    Value value(int depth);

private:
    struct Layout {
        std::vector<clone::Key> keys;
        std::vector<std::array<uint64_t, clone::kDefineCacheWords>> caches;
    };

    Value tagged(uint8_t tag, size_t slot, int depth);
    Value viewBuffer(int depth);
    bool str(std::string& out);
    clone::Key keyRef();
    Value plainObject(size_t slot, Layout* layout, uint32_t count, int depth);

    Reader r_;
    const Message& msg_;
    // The reader's side of the memory: each object made so far, numbered in
    // the writer's order. A container claims its number when its tag is read
    // and fills it once the object exists, before its children are read, so a
    // cycle back to it resolves.
    std::vector<ev::Persistent> objs_;
    std::vector<clone::Key> keys_;
    std::deque<Layout> layouts_;  // deque: a nested read's push keeps references valid
};

bool ReaderState::str(std::string& out) {
    if (!r_.ok(4)) return false;
    const uint32_t len = r_.u32();
    if (!r_.ok(len)) return false;
    out.assign(reinterpret_cast<const char*>(r_.ptr(len)), len);
    return true;
}

clone::Key ReaderState::keyRef() {
    if (!r_.ok(4)) ev::throwTypeError("postMessage: truncated key");
    const uint32_t id = r_.u32();
    if (id != kLiteralKey && id < keys_.size()) return keys_[id];
    if (id != kLiteralKey && id != keys_.size()) ev::throwTypeError("postMessage: invalid key reference");
    if (!r_.ok(5)) ev::throwTypeError("postMessage: truncated key");
    const bool utf16 = r_.u8() != 0;
    const uint32_t len = r_.u32();
    const size_t bytes = size_t(len) * (utf16 ? 2 : 1);
    if (!r_.ok(bytes)) ev::throwTypeError("postMessage: truncated key");
    clone::Key k = clone::internKey(r_.ptr(bytes), len, utf16);
    if (id != kLiteralKey) keys_.push_back(k);
    return k;
}

Value ReaderState::value(int depth) {
    if (depth > kMaxDepth) return ev::throwTypeError("postMessage: object too deeply nested");
    if (!r_.ok(1)) return ev::throwTypeError("postMessage: truncated data");
    const uint8_t tag = r_.u8();
    if (!makesObject(tag)) return tagged(tag, 0, depth);
    const size_t slot = objs_.size();
    objs_.emplace_back();
    Value v = tagged(tag, slot, depth);
    objs_[slot].set(v);  // no allocation
    return v;
}

Value ReaderState::viewBuffer(int depth) {
    Value ab = value(depth + 1);
    if (!ev::isArrayBuffer(ab)) return ev::throwTypeError("postMessage: view over a non-buffer");
    return ab;
}

// A plain object of `count` properties: from `layout` when one is given,
// else a key reference before each value.
Value ReaderState::plainObject(size_t slot, Layout* layout, uint32_t count, int depth) {
    objs_[slot].set(ev::createObject());
    for (uint32_t i = 0; i < count; ++i) {
        const clone::Key k = layout ? layout->keys[i] : keyRef();
        Value v = value(depth + 1);
        // No allocation between the read above and the define: `v` and the
        // root's value are both current at the call.
        uint64_t* cache = layout ? layout->caches[i].data() : nullptr;
        objs_[slot].set(clone::defineOwn(objs_[slot].get(), k, v, cache));
    }
    return objs_[slot].get();
}

Value ReaderState::tagged(uint8_t tag, size_t slot, int depth) {
    switch (tag) {
    case kUndefined: return ev::undefined();
    case kNull:      return ev::null();
    case kTrue:      return ev::fromBool(true);
    case kFalse:     return ev::fromBool(false);
    case kInt32:
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated int32");
        return ev::fromDouble(static_cast<int32_t>(r_.u32()));
    case kFloat64:
        if (!r_.ok(8)) return ev::throwTypeError("postMessage: truncated float64");
        return ev::fromDouble(r_.f64());
    case kStringLatin1:
    case kStringUtf16: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated string");
        const uint32_t len = r_.u32();
        const bool utf16 = tag == kStringUtf16;
        const size_t bytes = size_t(len) * (utf16 ? 2 : 1);
        if (!r_.ok(bytes)) return ev::throwTypeError("postMessage: truncated string");
        return clone::makeString(r_.ptr(bytes), len, utf16);
    }
    case kString: {
        std::string s;
        if (!str(s)) return ev::throwTypeError("postMessage: truncated string");
        return ev::fromUtf8(s);
    }
    case kBigInt: {
        std::string s;
        if (!str(s)) return ev::throwTypeError("postMessage: truncated bigint");
        ev::Persistent strRoot(ev::fromUtf8(s));
        Value ctor = getGlobal("BigInt");
        Value strV = strRoot.get();
        return ev::call(ctor, ev::undefined(), std::span<const Value>(&strV, 1)).value;
    }
    case kArrayBuffer: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated arraybuffer length");
        const uint32_t len = r_.u32();
        if (!r_.ok(len)) return ev::throwTypeError("postMessage: truncated arraybuffer data");
        return ev::createArrayBuffer(std::span<const uint8_t>(r_.ptr(len), len));
    }
    case kBufferBlock: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated buffer index");
        const uint32_t idx = r_.u32();
        if (idx >= msg_.buffers.size()) return ev::throwTypeError("postMessage: invalid buffer index");
        ByteBlock& block = msg_.buffers[idx];
        if (!block.data) {
            if (block.size != 0) return ev::throwTypeError("postMessage: buffer already received");
            return ev::createArrayBuffer(size_t(0));
        }
        // Adopted, not copied: bronze owns the block from here and frees it
        // with the buffer. One-shot, like a transfer.
        uint8_t* bytes = block.data;
        const uint32_t size = block.size;
        block.data = nullptr;
        return ev::createExternalArrayBuffer(bytes, size, freeBlock, nullptr);
    }
    case kObjectRef: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated object reference");
        const uint32_t idx = r_.u32();
        // A number is claimed before its object exists; no writer refers to
        // one still being built from inside it except a container, which is
        // filled first.
        if (idx >= objs_.size()) return ev::throwTypeError("postMessage: invalid object reference");
        return objs_[idx].get();
    }
    case kTransferImageBitmap: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated imagebitmap transfer index");
        const uint32_t idx = r_.u32();
        if (idx >= msg_.transferredImages.size()) return ev::throwTypeError("postMessage: invalid imagebitmap index");
        // The sender's pixels themselves (shared, immutable): no copy on the
        // receiving thread. In the page realm a big one starts its texture
        // upload here (wrapHostImageBitmap).
        const SerializedImage& simg = msg_.transferredImages[idx];
        return wrapHostImageBitmap(simg.pixels, simg.upload);
    }
    case kTransferMesh: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated mesh transfer index");
        const uint32_t idx = r_.u32();
#if BRO_WITH_3D
        if (idx >= msg_.transferredMeshes.size() || !msg_.transferredMeshes[idx]) {
            return ev::throwTypeError("postMessage: invalid mesh transfer index");
        }
        std::unique_ptr<bromesh::MeshData> data = std::move(msg_.transferredMeshes[idx]);
        return bromesh::api::makeMeshValue(std::move(*data));
#else
        (void)idx;
        return ev::throwTypeError("postMessage: Mesh transfer needs BRO_WITH_3D");
#endif
    }
    case kTypedArray: {
        if (!r_.ok(1 + 4 + 4)) return ev::throwTypeError("postMessage: truncated typed array header");
        const uint8_t subtype = r_.u8();
        const uint32_t offset = r_.u32();
        const uint32_t viewBytes = r_.u32();
        if (subtype > 11) return ev::throwTypeError("postMessage: invalid typed array kind");
        Value ab = viewBuffer(depth);
        return ev::createTypedArrayView(static_cast<bronze::ElementKind>(subtype), ab, offset,
                                        viewBytes / bytesPerElement(subtype));
    }
    case kDate:
        if (!r_.ok(8)) return ev::throwTypeError("postMessage: truncated date");
        return clone::newDate(r_.f64());
    case kRegExp: {
        std::string src, flags;
        if (!str(src) || !str(flags)) return ev::throwTypeError("postMessage: truncated regexp");
        ev::Persistent srcV(ev::fromUtf8(src));
        ev::Persistent flagsV(ev::fromUtf8(flags));
        Value ctor = getGlobal("RegExp");
        const Value args[2] = { srcV.get(), flagsV.get() };
        return ev::construct(ctor, std::span<const Value>(args, 2)).value;
    }
    case kMap:
    case kSet: {
        const bool isMap = tag == kMap;
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated collection length");
        const uint32_t len = r_.u32();
        if (len > r_.remaining()) return ev::throwTypeError("postMessage: truncated collection");
        objs_[slot].set(isMap ? clone::newMap() : clone::newSet());
        ev::Persistent keyRoot;
        for (uint32_t i = 0; i < len; ++i) {
            Value k = value(depth + 1);
            if (isMap) {
                keyRoot.set(k);
                Value v = value(depth + 1);
                objs_[slot].set(clone::collectionPut(objs_[slot].get(), keyRoot.get(), v));
            } else {
                objs_[slot].set(clone::collectionPut(objs_[slot].get(), k, k));
            }
        }
        return objs_[slot].get();
    }
    case kError: {
        std::string name, message, stack;
        if (!str(name) || !str(message) || !str(stack))
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
        ev::Persistent nameV(ev::fromUtf8(name));
        err.set(ev::setProperty(err.get(), "name", nameV.get()));
        ev::Persistent stackV(ev::fromUtf8(stack));
        err.set(ev::setProperty(err.get(), "stack", stackV.get()));
        return err.get();
    }
    case kDataView: {
        if (!r_.ok(4 + 4)) return ev::throwTypeError("postMessage: truncated dataview header");
        const uint32_t off = r_.u32();
        const uint32_t viewBytes = r_.u32();
        ev::Persistent ab(viewBuffer(depth));
        Value ctor = getGlobal("DataView");
        const Value args[3] = { ab.get(), ev::fromDouble(off), ev::fromDouble(viewBytes) };
        return ev::construct(ctor, std::span<const Value>(args, 3)).value;
    }
    case kArray: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated array length");
        const uint32_t len = r_.u32();
        if (len > r_.remaining()) return ev::throwTypeError("postMessage: truncated array");
        objs_[slot].set(clone::newArray(len));
        for (uint32_t i = 0; i < len; ++i) {
            Value v = value(depth + 1);
            objs_[slot].set(clone::arrayPut(objs_[slot].get(), i, v));
        }
        return objs_[slot].get();
    }
    case kObject: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated object prop count");
        const uint32_t count = r_.u32();
        if (count > r_.remaining() / 5) return ev::throwTypeError("postMessage: truncated object");
        return plainObject(slot, nullptr, count, depth);
    }
    case kObjectLayout: {
        if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated object layout");
        const uint32_t id = r_.u32();
        if (id > layouts_.size()) return ev::throwTypeError("postMessage: invalid object layout");
        if (id == layouts_.size()) {
            if (!r_.ok(4)) return ev::throwTypeError("postMessage: truncated object layout");
            const uint32_t n = r_.u32();
            if (n > r_.remaining() / 4) return ev::throwTypeError("postMessage: truncated object layout");
            Layout layout;
            layout.keys.reserve(n);
            for (uint32_t i = 0; i < n; ++i) layout.keys.push_back(keyRef());
            layout.caches.assign(n, {});
            layouts_.push_back(std::move(layout));
        }
        Layout& layout = layouts_[id];
        return plainObject(slot, &layout, static_cast<uint32_t>(layout.keys.size()), depth);
    }
    default:
        return ev::throwTypeError("postMessage: unknown tag");
    }
}

}  // namespace

Value deserializeMessage(const Message& msg, size_t offset) {
    if (offset > msg.data.size()) {
        return ev::throwTypeError("deserializeMessage: offset out of range");
    }
    ReaderState st(msg, offset);
    return st.value(0);
}

}  // namespace bro::bronze_host

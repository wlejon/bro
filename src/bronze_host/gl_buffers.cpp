// Buffers, vertex arrays and vertex attributes for the surface three.js's renderer
// actually drives (create/bind/data/subdata, VAOs, attribute pointers,
// instancing divisors, indexed uniform-buffer bindings).
//
// THE UPLOAD CONTRACT, because this file is where it bites hardest:
// bufferData and bufferSubData receive a pointer INTO THE MOVING BRONZE HEAP
// (embed::typedArrayInfo). Each hands it to the GL entry point in the very
// next statement — the driver copies the bytes synchronously — and nothing
// between the read and the call can allocate on the bronze heap. The pointer
// is never stored, and after the GL call it is treated as dead.

#include "bronze_host/gl_internal.h"

namespace bro::bronze_host {

static std::unordered_map<uint64_t, ev::Persistent> s_indexedBindings;

void stashIndexedBinding(uint32_t target, uint32_t index, Value bufVal) {
    uint64_t key = (static_cast<uint64_t>(target) << 32) | index;
    if (ev::isNull(bufVal) || ev::isUndefined(bufVal)) {
        s_indexedBindings.erase(key);
    } else {
        s_indexedBindings.insert_or_assign(key, ev::Persistent(bufVal));
    }
}

Value loadIndexedBinding(uint32_t target, uint32_t index) {
    uint64_t key = (static_cast<uint64_t>(target) << 32) | index;
    auto it = s_indexedBindings.find(key);
    if (it != s_indexedBindings.end()) {
        return it->second.get();
    }
    return ev::null();
}

void installGlBuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("createBuffer", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Buffer, live(c)->createBuffer().id);
    });
    b.def("deleteBuffer", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteBuffer({idOf(argAt(a, 0), GlCell::Buffer)});
        return ev::undefined();
    });
    b.def("bindBuffer", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindBuffer(u32At(a, 0), {idOf(argAt(a, 1), GlCell::Buffer)});
        return ev::undefined();
    });
    b.def("isBuffer", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isBuffer({idOf(argAt(a, 0), GlCell::Buffer)}) != GL_FALSE);
    });

    // Signatures: bufferData(target, size, usage), bufferData(target, data, usage),
    // and the WebGL2 form bufferData(target, srcData, usage, srcOffset[, length])
    // where srcOffset/length count ELEMENTS of the source view, not bytes.
    b.def("bufferData", 3, [c](Value, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLenum usage = u32At(a, 2);
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 1), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t srcOffset = static_cast<size_t>(u32At(a, 3));
            if (srcOffset > elemCount) srcOffset = elemCount;
            size_t count = elemCount - srcOffset;
            if (a.size() >= 5 && !ev::isUndefined(a[4])) {
                size_t l = static_cast<size_t>(u32At(a, 4));
                if (l < count) count = l;
            }
            // The one GL call this pointer lives for.
            live(c)->bufferData(target, static_cast<GLsizeiptr>(count * elemSize),
                                data + srcOffset * elemSize, usage);
        } else {
            live(c)->bufferData(target, static_cast<GLsizeiptr>(i64At(a, 1)), nullptr, usage);
        }
        return ev::undefined();
    });

    b.def("bufferSubData", 3, [c](Value, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLintptr dstOffset = static_cast<GLintptr>(i64At(a, 1));
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 2), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t srcOffset = static_cast<size_t>(u32At(a, 3));
            if (srcOffset > elemCount) srcOffset = elemCount;
            size_t count = elemCount - srcOffset;
            if (a.size() >= 5 && !ev::isUndefined(a[4])) {
                size_t l = static_cast<size_t>(u32At(a, 4));
                if (l < count) count = l;
            }
            live(c)->bufferSubData(target, dstOffset,
                                   static_cast<GLsizeiptr>(count * elemSize),
                                   data + srcOffset * elemSize);
        }
        return ev::undefined();
    });

    b.def("copyBufferSubData", 5, [c](Value, std::span<const Value> a) {
        live(c)->copyBufferSubData(u32At(a, 0), u32At(a, 1),
                                   static_cast<GLintptr>(i64At(a, 2)),
                                   static_cast<GLintptr>(i64At(a, 3)),
                                   static_cast<GLsizeiptr>(i64At(a, 4)));
        return ev::undefined();
    });

    // getBufferSubData(target, srcByteOffset, dstView [, dstOffset, length]) —
    // GL WRITES INTO the bronze heap here, which is safe under exactly the same
    // rule as reads: the destination pointer is taken and consumed with no
    // bronze allocation in between, and the GL call is synchronous.
    // dstOffset and length are in ELEMENT units of the destination view.
    b.def("getBufferSubData", 3, [c](Value, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLintptr srcOffset = static_cast<GLintptr>(i64At(a, 1));
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 2), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t dstOffset = static_cast<size_t>(u32At(a, 3));
            if (dstOffset > elemCount) dstOffset = elemCount;
            size_t count = elemCount - dstOffset;
            if (hasArg(a, 4)) {
                size_t l = static_cast<size_t>(u32At(a, 4));
                if (l < count) count = l;
            }
            live(c)->getBufferSubData(target, srcOffset,
                                      const_cast<uint8_t*>(data + dstOffset * elemSize),
                                      static_cast<GLsizeiptr>(count * elemSize));
        }
        return ev::undefined();
    });

    // --- Buffer mapping (BRO_buffer_map) ---
    // Returns an ArrayBuffer backed directly by driver memory. Detached on unmapBuffer.
    static std::unordered_map<GLuint, ev::Persistent> s_mappedBuffers;

    b.def("mapBufferRange", 4, [c](Value, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLintptr offset = static_cast<GLintptr>(i64At(a, 1));
        GLsizeiptr length = static_cast<GLsizeiptr>(i64At(a, 2));
        GLbitfield access = u32At(a, 3);
        void* ptr = live(c)->mapBufferRange(target, offset, length, access);
        if (!ptr) return ev::null();
        Value ab = ev::createExternalArrayBuffer(
            reinterpret_cast<uint8_t*>(ptr), static_cast<uint32_t>(length),
            [](void*, uint8_t*) {}, nullptr);
        GLuint bufId = live(c)->boundBuffer(target);
        s_mappedBuffers.insert_or_assign(bufId, ev::Persistent(ab));
        return ab;
    });

    b.def("unmapBuffer", 1, [c](Value, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLuint bufId = live(c)->boundBuffer(target);
        auto it = s_mappedBuffers.find(bufId);
        if (it != s_mappedBuffers.end()) {
            ev::detachArrayBuffer(it->second.get());
            s_mappedBuffers.erase(it);
        }
        return ev::fromBool(live(c)->unmapBuffer(target));
    });

    b.def("flushMappedBufferRange", 3, [c](Value, std::span<const Value> a) {
        live(c)->flushMappedBufferRange(u32At(a, 0), static_cast<GLintptr>(i64At(a, 1)),
                                         static_cast<GLsizeiptr>(i64At(a, 2)));
        return ev::undefined();
    });

    // The indexed forms.
    b.def("bindBufferBase", 3, [c](Value, std::span<const Value> a) {
        uint32_t target = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        Value bufVal = argAt(a, 2);
        live(c)->bindBufferBase(target, index, {idOf(bufVal, GlCell::Buffer)});
        stashIndexedBinding(target, index, bufVal);
        return ev::undefined();
    });
    b.def("bindBufferRange", 5, [c](Value, std::span<const Value> a) {
        uint32_t target = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        Value bufVal = argAt(a, 2);
        live(c)->bindBufferRange(target, index, {idOf(bufVal, GlCell::Buffer)},
                                 static_cast<GLintptr>(i64At(a, 3)),
                                 static_cast<GLsizeiptr>(i64At(a, 4)));
        stashIndexedBinding(target, index, bufVal);
        return ev::undefined();
    });

    // --- Vertex array objects ---
    b.def("createVertexArray", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::VertexArray, live(c)->createVertexArray().id);
    });
    b.def("deleteVertexArray", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteVertexArray({idOf(argAt(a, 0), GlCell::VertexArray)});
        return ev::undefined();
    });
    b.def("bindVertexArray", 1, [c](Value, std::span<const Value> a) {
        live(c)->bindVertexArray({idOf(argAt(a, 0), GlCell::VertexArray)});
        return ev::undefined();
    });
    b.def("isVertexArray", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isVertexArray({idOf(argAt(a, 0), GlCell::VertexArray)}) != GL_FALSE);
    });

    // --- Vertex attributes ---
    b.def("vertexAttribPointer", 6, [c](Value, std::span<const Value> a) {
        live(c)->vertexAttribPointer(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                     boolAt(a, 3) ? GL_TRUE : GL_FALSE, i32At(a, 4),
                                     static_cast<GLintptr>(i64At(a, 5)));
        return ev::undefined();
    });
    b.def("vertexAttribIPointer", 5, [c](Value, std::span<const Value> a) {
        live(c)->vertexAttribIPointer(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                                      static_cast<GLintptr>(i64At(a, 4)));
        return ev::undefined();
    });
    b.def("enableVertexAttribArray", 1, [c](Value, std::span<const Value> a) {
        live(c)->enableVertexAttribArray(u32At(a, 0));
        return ev::undefined();
    });
    b.def("disableVertexAttribArray", 1, [c](Value, std::span<const Value> a) {
        live(c)->disableVertexAttribArray(u32At(a, 0));
        return ev::undefined();
    });
    b.def("vertexAttribDivisor", 2, [c](Value, std::span<const Value> a) {
        live(c)->vertexAttribDivisor(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("vertexAttribI4i", 5, [c](Value, std::span<const Value> a) {
        live(c)->vertexAttribI4i(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
    b.def("vertexAttribI4ui", 5, [c](Value, std::span<const Value> a) {
        live(c)->vertexAttribI4ui(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3), u32At(a, 4));
        return ev::undefined();
    });
    b.def("vertexAttribI4iv", 2, [c](Value, std::span<const Value> a) {
        std::vector<int32_t> storage;
        const int32_t* data = nullptr;
        size_t count = 0;
        if (int32Data(argAt(a, 1), storage, &data, &count) && count >= 4) {
            live(c)->vertexAttribI4iv(u32At(a, 0), data);
        }
        return ev::undefined();
    });
    b.def("vertexAttribI4uiv", 2, [c](Value, std::span<const Value> a) {
        std::vector<uint32_t> storage;
        const uint32_t* data = nullptr;
        size_t count = 0;
        if (uint32Data(argAt(a, 1), storage, &data, &count) && count >= 4) {
            live(c)->vertexAttribI4uiv(u32At(a, 0), data);
        }
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host

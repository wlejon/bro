// Buffers, vertex arrays and vertex attributes — the bronze twin of
// src/js/webgl2_bindings_buffers.cpp for the surface three.js's renderer
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

    // Signatures, matching the QuickJS binding: bufferData(target, size,
    // usage), bufferData(target, data, usage), and the WebGL2 form
    // bufferData(target, srcData, usage, srcOffset[, length]) where
    // srcOffset/length count ELEMENTS of the source view, not bytes.
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

    // getBufferSubData(target, srcByteOffset, dstView) — GL WRITES INTO the
    // bronze heap here, which is safe under exactly the same rule as reads:
    // the destination pointer is taken and consumed with no bronze allocation
    // in between, and the GL call is synchronous.
    b.def("getBufferSubData", 3, [c](Value, std::span<const Value> a) {
        auto info = ev::typedArrayInfo(argAt(a, 2));
        if (info) {
            live(c)->getBufferSubData(u32At(a, 0), static_cast<GLintptr>(i64At(a, 1)),
                                      info.data, static_cast<GLsizeiptr>(info.byteLength));
        }
        return ev::undefined();
    });

    // The indexed forms. The QuickJS layer also stashes the wrapper object so
    // getIndexedParameter can answer it back; getIndexedParameter is not
    // bound here (left out, named in the module README), so no stash.
    b.def("bindBufferBase", 3, [c](Value, std::span<const Value> a) {
        live(c)->bindBufferBase(u32At(a, 0), u32At(a, 1), {idOf(argAt(a, 2), GlCell::Buffer)});
        return ev::undefined();
    });
    b.def("bindBufferRange", 5, [c](Value, std::span<const Value> a) {
        live(c)->bindBufferRange(u32At(a, 0), u32At(a, 1), {idOf(argAt(a, 2), GlCell::Buffer)},
                                 static_cast<GLintptr>(i64At(a, 3)),
                                 static_cast<GLsizeiptr>(i64At(a, 4)));
        return ev::undefined();
    });

    // --- Vertex array objects ---
    b.def("createVertexArray", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Vao, live(c)->createVertexArray().id);
    });
    b.def("deleteVertexArray", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteVertexArray({idOf(argAt(a, 0), GlCell::Vao)});
        return ev::undefined();
    });
    b.def("bindVertexArray", 1, [c](Value, std::span<const Value> a) {
        live(c)->bindVertexArray({idOf(argAt(a, 0), GlCell::Vao)});
        return ev::undefined();
    });
    b.def("isVertexArray", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isVertexArray({idOf(argAt(a, 0), GlCell::Vao)}) != GL_FALSE);
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
}

}  // namespace bro::bronze_host

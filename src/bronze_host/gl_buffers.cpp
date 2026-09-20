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

struct ContextBufferState {
    std::unordered_map<uint64_t, ev::Persistent> indexedBindings;
    std::unordered_map<GLuint, ev::Persistent> mappedBuffers;
};
static std::unordered_map<webgl::WebGL2RenderingContext*, ContextBufferState> s_contextBuffers;

void stashIndexedBinding(webgl::WebGL2RenderingContext* c, uint32_t target, uint32_t index, Value bufVal) {
    if (!c) return;
    uint64_t key = (static_cast<uint64_t>(target) << 32) | index;
    if (ev::isNull(bufVal) || ev::isUndefined(bufVal)) {
        auto it = s_contextBuffers.find(c);
        if (it != s_contextBuffers.end()) {
            it->second.indexedBindings.erase(key);
        }
    } else {
        s_contextBuffers[c].indexedBindings.insert_or_assign(key, ev::Persistent(bufVal));
    }
}

Value loadIndexedBinding(webgl::WebGL2RenderingContext* c, uint32_t target, uint32_t index) {
    if (!c) return ev::null();
    uint64_t key = (static_cast<uint64_t>(target) << 32) | index;
    auto ctxIt = s_contextBuffers.find(c);
    if (ctxIt != s_contextBuffers.end()) {
        auto it = ctxIt->second.indexedBindings.find(key);
        if (it != ctxIt->second.indexedBindings.end()) {
            return it->second.get();
        }
    }
    return ev::null();
}

void installGlBuffers(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    if (c) {
        c->addTeardownCallback([](webgl::WebGL2RenderingContext* ctx) {
            auto it = s_contextBuffers.find(ctx);
            if (it != s_contextBuffers.end()) {
                for (auto& [bufId, persistent] : it->second.mappedBuffers) {
                    ev::detachArrayBuffer(persistent.get());
                }
                s_contextBuffers.erase(it);
            }
        });
    }

    b.def("createBuffer", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Buffer, live(c)->createBuffer().id);
    });
    b.def("deleteBuffer", 1, [c](Value, std::span<const Value> a) {
        GLuint bufId = idOf(argAt(a, 0), GlCell::Buffer);
        if (bufId) {
            auto ctxIt = s_contextBuffers.find(c);
            if (ctxIt != s_contextBuffers.end()) {
                auto mIt = ctxIt->second.mappedBuffers.find(bufId);
                if (mIt != ctxIt->second.mappedBuffers.end()) {
                    ev::detachArrayBuffer(mIt->second.get());
                    ctxIt->second.mappedBuffers.erase(mIt);
                }
                for (auto it = ctxIt->second.indexedBindings.begin(); it != ctxIt->second.indexedBindings.end(); ) {
                    if (idOf(it->second.get(), GlCell::Buffer) == bufId) {
                        it = ctxIt->second.indexedBindings.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            live(c)->deleteBuffer({bufId});
        }
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
        s_contextBuffers[c].mappedBuffers.insert_or_assign(bufId, ev::Persistent(ab));
        return ab;
    });

    b.def("unmapBuffer", 1, [c](Value, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLuint bufId = live(c)->boundBuffer(target);
        auto ctxIt = s_contextBuffers.find(c);
        if (ctxIt != s_contextBuffers.end()) {
            auto it = ctxIt->second.mappedBuffers.find(bufId);
            if (it != ctxIt->second.mappedBuffers.end()) {
                ev::detachArrayBuffer(it->second.get());
                ctxIt->second.mappedBuffers.erase(it);
            }
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
        stashIndexedBinding(c, target, index, bufVal);
        return ev::undefined();
    });
    b.def("bindBufferRange", 5, [c](Value, std::span<const Value> a) {
        uint32_t target = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        Value bufVal = argAt(a, 2);
        live(c)->bindBufferRange(target, index, {idOf(bufVal, GlCell::Buffer)},
                                 static_cast<GLintptr>(i64At(a, 3)),
                                 static_cast<GLsizeiptr>(i64At(a, 4)));
        stashIndexedBinding(c, target, index, bufVal);
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

    b.def("vertexAttrib1f", 2, [c](Value, std::span<const Value> a) {
        live(c);
        glVertexAttrib1f(u32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("vertexAttrib2f", 3, [c](Value, std::span<const Value> a) {
        live(c);
        glVertexAttrib2f(u32At(a, 0), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    b.def("vertexAttrib3f", 4, [c](Value, std::span<const Value> a) {
        live(c);
        glVertexAttrib3f(u32At(a, 0), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                         static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("vertexAttrib4f", 5, [c](Value, std::span<const Value> a) {
        live(c);
        glVertexAttrib4f(u32At(a, 0), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                         static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)));
        return ev::undefined();
    });

    auto defAttribFv = [&](const char* name, size_t comps, void (*fn)(GLuint, const GLfloat*)) {
        b.def(name, 2, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<float> storage;
            const float* p = nullptr;
            size_t n = 0;
            if (floatData(argAt(a, 1), storage, &p, &n)) {
                size_t srcOffset = hasArg(a, 2) ? static_cast<size_t>(u32At(a, 2)) : 0;
                if (srcOffset <= n) {
                    size_t count = n - srcOffset;
                    if (hasArg(a, 3)) {
                        size_t l = static_cast<size_t>(u32At(a, 3));
                        if (l > 0 && l < count) count = l;
                    }
                    if (count >= comps) {
                        live(c);
                        fn(u32At(a, 0), p + srcOffset);
                    }
                }
            }
            return ev::undefined();
        });
    };
    defAttribFv("vertexAttrib1fv", 1, glVertexAttrib1fv);
    defAttribFv("vertexAttrib2fv", 2, glVertexAttrib2fv);
    defAttribFv("vertexAttrib3fv", 3, glVertexAttrib3fv);
    defAttribFv("vertexAttrib4fv", 4, glVertexAttrib4fv);
}

}  // namespace bro::bronze_host

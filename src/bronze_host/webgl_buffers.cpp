// Buffers, vertex arrays and vertex attributes for the surface three.js's renderer
// actually drives (create/bind/data/subdata, VAOs, attribute pointers,
// instancing divisors, indexed uniform-buffer bindings).
//
// THE UPLOAD CONTRACT, because this file is where it bites hardest:
// bufferData and bufferSubData receive a pointer INTO THE MOVING BRONZE HEAP
// (embed::typedArrayInfo). Each hands it to the client in the very next
// statement — the client copies the bytes into the command before returning,
// the backend sees only that copy, later — and nothing between the read and
// the call can allocate on the bronze heap. getBufferSubData writes through
// such a pointer inside a sync() round trip, which runs while this thread
// waits. The pointer is never stored, and after the call it is treated as
// dead.

#include "bronze_host/webgl_internal.h"

namespace bro::bronze_host {

namespace {

// The ArrayBuffers mapBufferRange handed out, by buffer: each points into
// the backend's memory, so it is detached when the mapping ends — unmapped,
// the buffer deleted, the context lost or destroyed.
std::unordered_map<webgl::WebGL2RenderingContext*, std::unordered_map<GLuint, ev::Persistent>> s_mappedBuffers;

void detachMapping(webgl::WebGL2RenderingContext* c, GLuint buffer) {
    auto ctxIt = s_mappedBuffers.find(c);
    if (ctxIt == s_mappedBuffers.end()) return;
    auto it = ctxIt->second.find(buffer);
    if (it == ctxIt->second.end()) return;
    ev::detachArrayBuffer(it->second.get());
    ctxIt->second.erase(it);
}

// The buffer bound to `target`, when any mapping is outstanding (a round
// trip only then): the one whose mapping a bufferData ends.
GLuint mappedBufferAt(webgl::WebGL2RenderingContext* c, webgl::WebGLClient* cl, GLenum target) {
    auto it = s_mappedBuffers.find(c);
    if (it == s_mappedBuffers.end() || it->second.empty()) return 0;
    return cl->sync([target](WebGLBackend& gl) { return gl.boundBuffer(target); });
}

}  // namespace

void detachWebGLMappings(webgl::WebGL2RenderingContext* c) {
    auto it = s_mappedBuffers.find(c);
    if (it == s_mappedBuffers.end()) return;
    for (auto& [buffer, mapping] : it->second) ev::detachArrayBuffer(mapping.get());
    s_mappedBuffers.erase(it);
}

void installWebGLBuffers(ObjectBuilder& b) {

    defMethod(b, "createBuffer", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createBuffer(id);
        return webglObject(c, WebGLCell::Buffer, id);
    });
    defMethod(b, "deleteBuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        GLuint bufId = idOf(argAt(a, 0), WebGLCell::Buffer);
        if (bufId) {
            detachMapping(c, bufId);
            if (auto* cl = client(c)) cl->deleteBuffer(webgl::WebGLBuffer{bufId});
            forgetWebGLObject(c, WebGLCell::Buffer, bufId);
        }
        return ev::undefined();
    });
    defMethod(b, "bindBuffer", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->bindBuffer(u32At(a, 0), webgl::WebGLBuffer{idOf(argAt(a, 1), WebGLCell::Buffer)});
        return ev::undefined();
    });
    defMethod(b, "isBuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const webgl::WebGLBuffer buf{idOf(argAt(a, 0), WebGLCell::Buffer)};
        return ev::fromBool(cl && cl->sync([buf](WebGLBackend& gl) { return gl.isBuffer(buf); }) != GL_FALSE);
    });

    // Signatures: bufferData(target, size, usage), bufferData(target, data, usage),
    // and the WebGL2 form bufferData(target, srcData, usage, srcOffset[, length])
    // where srcOffset/length count ELEMENTS of the source view, not bytes.
    defMethod(b, "bufferData", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        GLenum target = u32At(a, 0);
        GLenum usage = u32At(a, 2);
        auto* cl = client(c);
        if (!cl) return ev::undefined();
        // bufferData unmaps a mapped buffer, so its ArrayBuffer ends here.
        if (const GLuint mapped = mappedBufferAt(c, cl, target)) detachMapping(c, mapped);
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
            // The one call this pointer lives for: the bytes are copied now.
            cl->bufferData(target, static_cast<GLsizeiptr>(count * elemSize), data + srcOffset * elemSize, usage);
        } else {
            cl->bufferData(target, static_cast<GLsizeiptr>(i64At(a, 1)), nullptr, usage);
        }
        return ev::undefined();
    });

    defMethod(b, "bufferSubData", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
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
            if (auto* cl = client(c))
                cl->bufferSubData(target, dstOffset, static_cast<GLsizeiptr>(count * elemSize),
                                  data + srcOffset * elemSize);
        }
        return ev::undefined();
    });

    defMethod(b, "copyBufferSubData", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->copyBufferSubData(u32At(a, 0), u32At(a, 1),
                                  static_cast<GLintptr>(i64At(a, 2)),
                                  static_cast<GLintptr>(i64At(a, 3)),
                                  static_cast<GLsizeiptr>(i64At(a, 4)));
        return ev::undefined();
    });

    // getBufferSubData(target, srcByteOffset, dstView [, dstOffset, length]) —
    // GL WRITES INTO the bronze heap here, inside a sync() round trip: the
    // destination pointer is taken and consumed with no bronze allocation in
    // between, and this thread waits until the backend has written it.
    // dstOffset and length are in ELEMENT units of the destination view.
    defMethod(b, "getBufferSubData", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
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
            uint8_t* dst = const_cast<uint8_t*>(data + dstOffset * elemSize);
            const GLsizeiptr length = static_cast<GLsizeiptr>(count * elemSize);
            if (auto* cl = client(c))
                cl->sync([&](WebGLBackend& gl) { gl.getBufferSubData(target, srcOffset, dst, length); });
        }
        return ev::undefined();
    });

    // --- Buffer mapping (BRO_buffer_map) ---
    // Returns an ArrayBuffer over the buffer's mapped memory, detached when
    // the mapping ends.
    defMethod(b, "mapBufferRange", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        GLenum target = u32At(a, 0);
        GLintptr offset = static_cast<GLintptr>(i64At(a, 1));
        GLsizeiptr length = static_cast<GLsizeiptr>(i64At(a, 2));
        GLbitfield access = u32At(a, 3);
        // The memory is the backend's buffer shadow, which it does not read
        // while the buffer is mapped (GL forbids using a mapped buffer).
        GLuint bufId = 0;
        void* ptr = cl->sync([&](WebGLBackend& gl) {
            void* p = gl.mapBufferRange(target, offset, length, access);
            bufId = gl.boundBuffer(target);
            return p;
        });
        if (!ptr) return ev::null();
        ev::Persistent ab(ev::createExternalArrayBuffer(reinterpret_cast<uint8_t*>(ptr),
                                                        static_cast<uint32_t>(length), [](void*, uint8_t*) {},
                                                        nullptr));
        Value out = ab.get();
        s_mappedBuffers[c].insert_or_assign(bufId, std::move(ab));
        return out;
    });

    defMethod(b, "unmapBuffer", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::fromBool(false);
        GLenum target = u32At(a, 0);
        GLuint bufId = 0;
        const bool ok = cl->sync([&](WebGLBackend& gl) {
            bufId = gl.boundBuffer(target);
            return gl.unmapBuffer(target);
        });
        detachMapping(c, bufId);
        return ev::fromBool(ok);
    });

    defMethod(b, "flushMappedBufferRange", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        // A round trip: the backend reads the mapped memory the page is
        // writing, so it reads it now, as the page sees it.
        const GLenum target = u32At(a, 0);
        const GLintptr offset = static_cast<GLintptr>(i64At(a, 1));
        const GLsizeiptr length = static_cast<GLsizeiptr>(i64At(a, 2));
        if (auto* cl = client(c))
            cl->sync([&](WebGLBackend& gl) { gl.flushMappedBufferRange(target, offset, length); });
        return ev::undefined();
    });

    // The indexed forms.
    defMethod(b, "bindBufferBase", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        uint32_t target = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        Value bufVal = argAt(a, 2);
        if (auto* cl = client(c)) cl->bindBufferBase(target, index, webgl::WebGLBuffer{idOf(bufVal, WebGLCell::Buffer)});
        return ev::undefined();
    });
    defMethod(b, "bindBufferRange", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        uint32_t target = u32At(a, 0);
        uint32_t index = u32At(a, 1);
        Value bufVal = argAt(a, 2);
        if (auto* cl = client(c))
            cl->bindBufferRange(target, index, webgl::WebGLBuffer{idOf(bufVal, WebGLCell::Buffer)},
                                static_cast<GLintptr>(i64At(a, 3)),
                                static_cast<GLsizeiptr>(i64At(a, 4)));
        return ev::undefined();
    });

    // --- Vertex array objects ---
    defMethod(b, "createVertexArray", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->createVertexArray(id);
        return webglObject(c, WebGLCell::VertexArray, id);
    });
    defMethod(b, "deleteVertexArray", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::VertexArray);
        if (auto* cl = client(c)) cl->deleteVertexArray(webgl::WebGLVertexArrayObject{id});
        forgetWebGLObject(c, WebGLCell::VertexArray, id);
        return ev::undefined();
    });
    defMethod(b, "bindVertexArray", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->bindVertexArray(webgl::WebGLVertexArrayObject{idOf(argAt(a, 0), WebGLCell::VertexArray)});
        return ev::undefined();
    });
    defMethod(b, "isVertexArray", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const webgl::WebGLVertexArrayObject vao{idOf(argAt(a, 0), WebGLCell::VertexArray)};
        return ev::fromBool(cl && cl->sync([vao](WebGLBackend& gl) { return gl.isVertexArray(vao); }) != GL_FALSE);
    });

    // --- Vertex attributes ---
    defMethod(b, "vertexAttribPointer", 6, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttribPointer(u32At(a, 0), i32At(a, 1), u32At(a, 2), boolAt(a, 3) ? GL_TRUE : GL_FALSE,
                                    i32At(a, 4), static_cast<uintptr_t>(i64At(a, 5)));
        return ev::undefined();
    });
    defMethod(b, "vertexAttribIPointer", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttribIPointer(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                                     static_cast<uintptr_t>(i64At(a, 4)));
        return ev::undefined();
    });
    defMethod(b, "enableVertexAttribArray", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->enableVertexAttribArray(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "disableVertexAttribArray", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->disableVertexAttribArray(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "vertexAttribDivisor", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->vertexAttribDivisor(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "vertexAttribI4i", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttribI4i(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3), i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "vertexAttribI4ui", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttribI4ui(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3), u32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "vertexAttribI4iv", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<int32_t, 16> storage;
        const int32_t* data = nullptr;
        size_t count = 0;
        if (int32Data(argAt(a, 1), storage, &data, &count) && count >= 4) {
            if (auto* cl = client(c)) cl->vertexAttribI4iv(u32At(a, 0), data, 4);
        }
        return ev::undefined();
    });
    defMethod(b, "vertexAttribI4uiv", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        SmallBuffer<uint32_t, 16> storage;
        const uint32_t* data = nullptr;
        size_t count = 0;
        if (uint32Data(argAt(a, 1), storage, &data, &count) && count >= 4) {
            if (auto* cl = client(c)) cl->vertexAttribI4uiv(u32At(a, 0), data, 4);
        }
        return ev::undefined();
    });

    defMethod(b, "vertexAttrib1f", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->vertexAttrib1f(u32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    defMethod(b, "vertexAttrib2f", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttrib2f(u32At(a, 0), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    defMethod(b, "vertexAttrib3f", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttrib3f(u32At(a, 0), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                               static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    defMethod(b, "vertexAttrib4f", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->vertexAttrib4f(u32At(a, 0), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                               static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)));
        return ev::undefined();
    });

    auto defAttribFv = [&](const char* name, size_t comps,
                           void (webgl::WebGLClient::*fn)(GLuint, const GLfloat*, size_t)) {
        defMethod(b, name, 2, [comps, fn](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
            SmallBuffer<float, 16> storage;
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
                    auto* cl = client(c);
                    if (cl && count >= comps) (cl->*fn)(u32At(a, 0), p + srcOffset, comps);
                }
            }
            return ev::undefined();
        });
    };
    defAttribFv("vertexAttrib1fv", 1, &webgl::WebGLClient::vertexAttrib1fv);
    defAttribFv("vertexAttrib2fv", 2, &webgl::WebGLClient::vertexAttrib2fv);
    defAttribFv("vertexAttrib3fv", 3, &webgl::WebGLClient::vertexAttrib3fv);
    defAttribFv("vertexAttrib4fv", 4, &webgl::WebGLClient::vertexAttrib4fv);
}

}  // namespace bro::bronze_host

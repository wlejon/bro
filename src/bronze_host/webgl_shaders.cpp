// Shaders, programs and uniforms for the surface three.js's program
// system drives: compile/link/introspect, uniform location lookup, scalar and
// vector uniform uploads, and the square matrix uploads WebGLUniforms leans
// on.
//
// The *v upload paths read their data through floatData()/int32Data(): a
// typed array answers a borrowed bronze-heap pointer consumed by the backend
// in the next statement (nothing between them allocates); a plain JS array is
// copied into host storage first, because reading its elements goes through
// embed property reads, which allocate — the copy is what makes the pointer
// the backend sees immune to that.

#include "bronze_host/webgl_internal.h"

#include <string>

namespace bro::bronze_host {

namespace {

// Builds the {name, type, size} object getActiveUniform/getActiveAttrib
// answer — WebGLActiveInfo's shape. `info.name` is host memory, so it
// survives the allocations the object build performs.
Value makeActiveInfo(const webgl::WebGLActiveInfo& info) {
    if (info.name.empty() && info.type == 0) return ev::null();
    ObjectBuilder o;
    Value nameV = ev::fromUtf8(info.name);
    o.set("name", nameV);
    o.set("type", ev::fromDouble(info.type));
    o.set("size", ev::fromDouble(info.size));
    return o.get();
}

}  // namespace

void installWebGLShaders(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    // --- Shaders ---
    b.def("createShader", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        webgl::WebGLShader s = gl->createShader(u32At(a, 0));
        return webglObject(c, WebGLCell::Shader, s.id, s.type);
    });
    b.def("deleteShader", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        if (cell) {
            const GLuint id = cell->id;
            if (auto* gl = live(c)) gl->deleteShader({id, cell->shaderType});
            forgetWebGLObject(c, WebGLCell::Shader, id);
        }
        return ev::undefined();
    });
    b.def("shaderSource", 2, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        Value src = argAt(a, 1);
        if (cell && !ev::isObject(src)) {
            if (auto* gl = live(c)) gl->shaderSource({cell->id, cell->shaderType}, ev::toUtf8(src));
        }
        return ev::undefined();
    });
    b.def("getShaderSource", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* gl = live(c);
        if (!gl || !cell) return ev::null();
        return ev::fromUtf8(gl->getShaderSource({cell->id, cell->shaderType}));
    });
    b.def("compileShader", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* gl = live(c);
        if (gl && cell) gl->compileShader({cell->id, cell->shaderType});
        return ev::undefined();
    });
    b.def("getShaderParameter", 2, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* gl = live(c);
        if (!gl || !cell) return ev::null();
        const GLenum pname = u32At(a, 1);
        switch (pname) {
            case GL_COMPILE_STATUS:
            case GL_DELETE_STATUS:
                return ev::fromBool(gl->getShaderParameter({cell->id, cell->shaderType}, pname) != GL_FALSE);
            case GL_SHADER_TYPE:
                return ev::fromDouble(cell->shaderType);
            default:
                gl->setSyntheticError(GL_INVALID_ENUM);
                return ev::null();
        }
    });
    b.def("getShaderInfoLog", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* gl = live(c);
        if (!gl || !cell) return ev::null();
        return ev::fromUtf8(gl->getShaderInfoLog({cell->id, cell->shaderType}));
    });
    b.def("isShader", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* gl = live(c);
        return ev::fromBool(gl && cell && gl->isShader({cell->id, cell->shaderType}) != GL_FALSE);
    });

    // --- Programs ---
    b.def("createProgram", 0, [c](Value, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::Program, gl->createProgram().id) : ev::null();
    });
    b.def("deleteProgram", 1, [c](Value, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Program);
        if (auto* gl = live(c)) gl->deleteProgram({id});
        forgetWebGLObject(c, WebGLCell::Program, id);
        return ev::undefined();
    });
    b.def("attachShader", 2, [c](Value, std::span<const Value> a) {
        auto* sh = cellOf(argAt(a, 1), WebGLCell::Shader);
        auto* gl = live(c);
        if (gl && sh) gl->attachShader({idOf(argAt(a, 0), WebGLCell::Program)}, {sh->id, sh->shaderType});
        return ev::undefined();
    });
    b.def("detachShader", 2, [c](Value, std::span<const Value> a) {
        auto* sh = cellOf(argAt(a, 1), WebGLCell::Shader);
        auto* gl = live(c);
        if (gl && sh) gl->detachShader({idOf(argAt(a, 0), WebGLCell::Program)}, {sh->id, sh->shaderType});
        return ev::undefined();
    });
    b.def("getAttachedShaders", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        const std::vector<GLuint> ids = gl->attachedShaders({idOf(argAt(a, 0), WebGLCell::Program)});
        std::vector<GLenum> types;
        for (GLuint id : ids)
            types.push_back(static_cast<GLenum>(gl->getShaderParameter({id, 0}, GL_SHADER_TYPE)));
        return hostArrayOf(ids.size(),
                           [&](size_t i) { return webglObject(c, WebGLCell::Shader, ids[i], types[i]); });
    });
    b.def("linkProgram", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->linkProgram({idOf(argAt(a, 0), WebGLCell::Program)});
        return ev::undefined();
    });
    b.def("validateProgram", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->validateProgram({idOf(argAt(a, 0), WebGLCell::Program)});
        return ev::undefined();
    });
    b.def("useProgram", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->useProgram({idOf(argAt(a, 0), WebGLCell::Program)});
        return ev::undefined();
    });
    b.def("getProgramParameter", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        const webgl::WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        const GLenum pname = u32At(a, 1);
        const GLint value = gl->getProgramParameter(p, pname);
        switch (pname) {
            case GL_LINK_STATUS:
            case GL_DELETE_STATUS:
            case GL_VALIDATE_STATUS:
                return ev::fromBool(value != 0);
            default:
                return ev::fromDouble(value);
        }
    });
    b.def("getProgramInfoLog", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        return ev::fromUtf8(gl->getProgramInfoLog({idOf(argAt(a, 0), WebGLCell::Program)}));
    });
    b.def("isProgram", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isProgram({idOf(argAt(a, 0), WebGLCell::Program)}) != GL_FALSE);
    });

    // --- Locations and introspection ---
    b.def("bindAttribLocation", 3, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 2);
        auto* gl = live(c);
        if (gl && !ev::isObject(name))
            gl->bindAttribLocation({idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1), ev::toUtf8(name));
        return ev::undefined();
    });
    b.def("getAttribLocation", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        auto* gl = live(c);
        if (!gl || ev::isObject(name)) return ev::fromDouble(-1);
        return ev::fromDouble(gl->getAttribLocation({idOf(argAt(a, 0), WebGLCell::Program)}, ev::toUtf8(name)));
    });
    b.def("getFragDataLocation", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        auto* gl = live(c);
        if (!gl || ev::isObject(name)) return ev::fromDouble(-1);
        return ev::fromDouble(gl->getFragDataLocation({idOf(argAt(a, 0), WebGLCell::Program)}, ev::toUtf8(name)));
    });
    b.def("getUniformLocation", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        auto* gl = live(c);
        if (!gl || ev::isObject(name)) return ev::null();
        return wrapUniformLocation(
            gl->getUniformLocation({idOf(argAt(a, 0), WebGLCell::Program)}, ev::toUtf8(name)));
    });
    b.def("getActiveAttrib", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        return makeActiveInfo(gl->getActiveAttrib({idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1)));
    });
    b.def("getActiveUniform", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        return makeActiveInfo(gl->getActiveUniform({idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1)));
    });
    b.def("getUniformBlockIndex", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        auto* gl = live(c);
        if (!gl || ev::isObject(name)) return ev::fromDouble(4294967295.0);  // INVALID_INDEX
        return ev::fromDouble(gl->getUniformBlockIndex({idOf(argAt(a, 0), WebGLCell::Program)}, ev::toUtf8(name)));
    });
    b.def("getActiveUniformBlockName", 2, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        return ev::fromUtf8(gl->getActiveUniformBlockName({idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1)));
    });
    b.def("getUniformIndices", 2, [c](Value, std::span<const Value> a) {
        const webgl::WebGLProgram prog{idOf(argAt(a, 0), WebGLCell::Program)};
        Value namesVal = argAt(a, 1);
        if (!live(c) || !ev::isObject(namesVal)) return ev::null();
        ev::Persistent root(namesVal);
        Value lenV = ev::getProperty(root.get(), "length");
        if (ev::isUndefined(lenV) || ev::isObject(lenV)) return ev::null();
        uint32_t n = 0;
        if (!lengthWithin(ev::toDouble(lenV), kMaxHostListLength, n)) return ev::null();
        std::vector<std::string> names;
        names.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            names.push_back(ev::toUtf8(ev::getElement(root.get(), i)));
        }
        // Re-fetched: a property getter on the list may have lost the context.
        auto* gl = live(c);
        if (!gl) return ev::null();
        auto indices = gl->getUniformIndices(prog, names);
        return hostArrayOf(indices.size(), [&indices](size_t i) { return ev::fromDouble(indices[i]); });
    });
    b.def("getActiveUniforms", 3, [c](Value, std::span<const Value> a) {
        const webgl::WebGLProgram prog{idOf(argAt(a, 0), WebGLCell::Program)};
        uint32_t pname = u32At(a, 2);
        std::vector<uint32_t> storage;
        const uint32_t* data = nullptr;
        size_t count = 0;
        if (!uint32Data(argAt(a, 1), storage, &data, &count)) return ev::null();
        auto* gl = live(c);
        if (!gl) return ev::null();
        auto params = gl->getActiveUniforms(prog, std::vector<GLuint>(data, data + count), pname);
        return hostArrayOf(params.size(), [&params, pname](size_t i) {
            if (pname == 0x8A3E /* UNIFORM_IS_ROW_MAJOR */) {
                return ev::fromBool(params[i] != 0);
            }
            return ev::fromDouble(params[i]);
        });
    });
    b.def("getActiveUniformBlockParameter", 3, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        if (!gl) return ev::null();
        const webgl::WebGLProgram prog{idOf(argAt(a, 0), WebGLCell::Program)};
        uint32_t blockIndex = u32At(a, 1);
        uint32_t pname = u32At(a, 2);
        switch (pname) {
            case 0x8A43: {  // UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES
                auto indices = gl->getActiveUniformBlockIndices(prog, blockIndex);
                return hostArrayOf(indices.size(), [&indices](size_t i) {
                    return ev::fromDouble(static_cast<uint32_t>(indices[i]));
                });
            }
            case 0x8A44:  // UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER
            case 0x8A46:  // UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER
                return ev::fromBool(gl->getActiveUniformBlockParameteri(prog, blockIndex, pname) != 0);
            default:
                return ev::fromDouble(gl->getActiveUniformBlockParameteri(prog, blockIndex, pname));
        }
    });
    b.def("uniformBlockBinding", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->uniformBlockBinding({idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });

    // --- Scalar uniforms ---
    b.def("uniform1f", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform1f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("uniform2f", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->uniform2f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                          static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    b.def("uniform3f", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->uniform3f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("uniform4f", 5, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->uniform4f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)),
                          static_cast<float>(numAt(a, 4)));
        return ev::undefined();
    });
    b.def("uniform1i", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform1i(locOf(argAt(a, 0)), i32At(a, 1));
        return ev::undefined();
    });
    b.def("uniform2i", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform2i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("uniform3i", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform3i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("uniform4i", 5, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->uniform4i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                          i32At(a, 4));
        return ev::undefined();
    });
    b.def("uniform1ui", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform1ui(locOf(argAt(a, 0)), u32At(a, 1));
        return ev::undefined();
    });
    b.def("uniform2ui", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform2ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    b.def("uniform3ui", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->uniform3ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("uniform4ui", 5, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->uniform4ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2), u32At(a, 3),
                           u32At(a, 4));
        return ev::undefined();
    });

    // --- Vector uniforms. One shape per element type; the component count
    //     divides the data length into the uniform `count`.
    //     Registration below stays a fixed def() sequence. ---
    using Ctx = WebGLBackend;
    auto defFv = [&](const char* name, int comps,
                     void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLfloat*)) {
        b.def(name, 2, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<float> storage;
            const float* p = nullptr;
            size_t n = 0;
            if (floatData(argAt(a, 1), storage, &p, &n)) {
                size_t srcOffset = hasArg(a, 2) ? static_cast<size_t>(u32At(a, 2)) : 0;
                if (srcOffset > n) srcOffset = n;
                size_t count = n - srcOffset;
                if (hasArg(a, 3)) {
                    size_t l = static_cast<size_t>(u32At(a, 3));
                    if (l > 0 && l < count) count = l;
                }
                auto* gl = live(c);
                if (gl && count >= static_cast<size_t>(comps))
                    (gl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(count / comps), p + srcOffset);
            }
            return ev::undefined();
        });
    };
    auto defIv = [&](const char* name, int comps,
                     void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLint*)) {
        b.def(name, 2, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<int32_t> storage;
            const int32_t* p = nullptr;
            size_t n = 0;
            if (int32Data(argAt(a, 1), storage, &p, &n)) {
                size_t srcOffset = hasArg(a, 2) ? static_cast<size_t>(u32At(a, 2)) : 0;
                if (srcOffset > n) srcOffset = n;
                size_t count = n - srcOffset;
                if (hasArg(a, 3)) {
                    size_t l = static_cast<size_t>(u32At(a, 3));
                    if (l > 0 && l < count) count = l;
                }
                auto* gl = live(c);
                if (gl && count >= static_cast<size_t>(comps))
                    (gl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(count / comps), p + srcOffset);
            }
            return ev::undefined();
        });
    };
    auto defUiv = [&](const char* name, int comps,
                      void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLuint*)) {
        b.def(name, 2, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<uint32_t> storage;
            const uint32_t* p = nullptr;
            size_t n = 0;
            if (uint32Data(argAt(a, 1), storage, &p, &n)) {
                size_t srcOffset = hasArg(a, 2) ? static_cast<size_t>(u32At(a, 2)) : 0;
                if (srcOffset > n) srcOffset = n;
                size_t count = n - srcOffset;
                if (hasArg(a, 3)) {
                    size_t l = static_cast<size_t>(u32At(a, 3));
                    if (l > 0 && l < count) count = l;
                }
                auto* gl = live(c);
                if (gl && count >= static_cast<size_t>(comps))
                    (gl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(count / comps), p + srcOffset);
            }
            return ev::undefined();
        });
    };
    defFv("uniform1fv", 1, &Ctx::uniform1fv);
    defFv("uniform2fv", 2, &Ctx::uniform2fv);
    defFv("uniform3fv", 3, &Ctx::uniform3fv);
    defFv("uniform4fv", 4, &Ctx::uniform4fv);
    defIv("uniform1iv", 1, &Ctx::uniform1iv);
    defIv("uniform2iv", 2, &Ctx::uniform2iv);
    defIv("uniform3iv", 3, &Ctx::uniform3iv);
    defIv("uniform4iv", 4, &Ctx::uniform4iv);
    defUiv("uniform1uiv", 1, &Ctx::uniform1uiv);
    defUiv("uniform2uiv", 2, &Ctx::uniform2uiv);
    defUiv("uniform3uiv", 3, &Ctx::uniform3uiv);
    defUiv("uniform4uiv", 4, &Ctx::uniform4uiv);

    // --- Matrix uniforms: uniformMatrix{N}fv / {C}x{R}fv(loc, transpose, data). ---
    auto defMat = [&](const char* name, int comps,
                      void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, GLboolean,
                                      const GLfloat*)) {
        b.def(name, 3, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<float> storage;
            const float* p = nullptr;
            size_t n = 0;
            if (floatData(argAt(a, 2), storage, &p, &n)) {
                size_t srcOffset = hasArg(a, 3) ? static_cast<size_t>(u32At(a, 3)) : 0;
                if (srcOffset > n) srcOffset = n;
                size_t count = n - srcOffset;
                if (hasArg(a, 4)) {
                    size_t l = static_cast<size_t>(u32At(a, 4));
                    if (l > 0 && l < count) count = l;
                }
                auto* gl = live(c);
                if (gl && count >= static_cast<size_t>(comps))
                    (gl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(count / comps),
                              boolAt(a, 1) ? GL_TRUE : GL_FALSE, p + srcOffset);
            }
            return ev::undefined();
        });
    };
    defMat("uniformMatrix2fv", 4, &Ctx::uniformMatrix2fv);
    defMat("uniformMatrix3fv", 9, &Ctx::uniformMatrix3fv);
    defMat("uniformMatrix4fv", 16, &Ctx::uniformMatrix4fv);
    defMat("uniformMatrix2x3fv", 6, &Ctx::uniformMatrix2x3fv);
    defMat("uniformMatrix3x2fv", 6, &Ctx::uniformMatrix3x2fv);
    defMat("uniformMatrix2x4fv", 8, &Ctx::uniformMatrix2x4fv);
    defMat("uniformMatrix4x2fv", 8, &Ctx::uniformMatrix4x2fv);
    defMat("uniformMatrix3x4fv", 12, &Ctx::uniformMatrix3x4fv);
    defMat("uniformMatrix4x3fv", 12, &Ctx::uniformMatrix4x3fv);
}

}  // namespace bro::bronze_host

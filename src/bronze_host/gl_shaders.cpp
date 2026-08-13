// Shaders, programs and uniforms — the bronze twin of
// src/js/webgl2_bindings_shaders.cpp for the surface three.js's program
// system drives: compile/link/introspect, uniform location lookup, scalar and
// vector uniform uploads, and the square matrix uploads WebGLUniforms leans
// on.
//
// The *v upload paths read their data through floatData()/int32Data(): a
// typed array answers a borrowed bronze-heap pointer consumed by the GL call
// in the next statement (nothing between them allocates); a plain JS array is
// copied into host storage first, because reading its elements goes through
// embed property reads, which allocate — the copy is what makes the pointer
// the GL call sees immune to that.

#include "bronze_host/gl_internal.h"

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

void installGlShaders(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    // --- Shaders ---
    b.def("createShader", 1, [c](Value, std::span<const Value> a) {
        GLenum type = u32At(a, 0);
        webgl::WebGLShader s = live(c)->createShader(type);
        return wrapGlObj(GlCell::Shader, s.id, s.type);
    });
    b.def("deleteShader", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), GlCell::Shader);
        if (cell) live(c)->deleteShader({cell->id, cell->shaderType});
        return ev::undefined();
    });
    b.def("shaderSource", 2, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), GlCell::Shader);
        Value src = argAt(a, 1);
        if (cell && !ev::isObject(src)) {
            live(c)->shaderSource({cell->id, cell->shaderType}, ev::toUtf8(src));
        }
        return ev::undefined();
    });
    b.def("compileShader", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), GlCell::Shader);
        if (cell) live(c)->compileShader({cell->id, cell->shaderType});
        return ev::undefined();
    });
    b.def("getShaderParameter", 2, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), GlCell::Shader);
        GLenum pname = u32At(a, 1);
        if (!cell) return ev::null();
        switch (pname) {
            case 0x8B81:  // COMPILE_STATUS
                return ev::fromBool(
                    live(c)->getShaderParameter_compileStatus({cell->id, cell->shaderType}) !=
                    GL_FALSE);
            case 0x8B4F:  // SHADER_TYPE — answered from the cell, like the wrapper struct
                return ev::fromDouble(cell->shaderType);
            case 0x8B80:  // DELETE_STATUS — nothing here defers deletion
                return ev::fromBool(false);
            default:
                return ev::null();
        }
    });
    b.def("getShaderInfoLog", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), GlCell::Shader);
        if (!cell) return ev::fromUtf8("");
        return ev::fromUtf8(live(c)->getShaderInfoLog({cell->id, cell->shaderType}));
    });
    b.def("isShader", 1, [c](Value, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), GlCell::Shader);
        return ev::fromBool(cell &&
                            live(c)->isShader({cell->id, cell->shaderType}) != GL_FALSE);
    });

    // --- Programs ---
    b.def("createProgram", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Program, live(c)->createProgram().id);
    });
    b.def("deleteProgram", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteProgram({idOf(argAt(a, 0), GlCell::Program)});
        return ev::undefined();
    });
    b.def("attachShader", 2, [c](Value, std::span<const Value> a) {
        auto* sh = cellOf(argAt(a, 1), GlCell::Shader);
        if (sh) {
            live(c)->attachShader({idOf(argAt(a, 0), GlCell::Program)},
                                  {sh->id, sh->shaderType});
        }
        return ev::undefined();
    });
    b.def("detachShader", 2, [c](Value, std::span<const Value> a) {
        auto* sh = cellOf(argAt(a, 1), GlCell::Shader);
        if (sh) {
            live(c)->detachShader({idOf(argAt(a, 0), GlCell::Program)},
                                  {sh->id, sh->shaderType});
        }
        return ev::undefined();
    });
    b.def("linkProgram", 1, [c](Value, std::span<const Value> a) {
        live(c)->linkProgram({idOf(argAt(a, 0), GlCell::Program)});
        return ev::undefined();
    });
    b.def("useProgram", 1, [c](Value, std::span<const Value> a) {
        live(c)->useProgram({idOf(argAt(a, 0), GlCell::Program)});
        return ev::undefined();
    });
    b.def("getProgramParameter", 2, [c](Value, std::span<const Value> a) {
        webgl::WebGLProgram p{idOf(argAt(a, 0), GlCell::Program)};
        GLenum pname = u32At(a, 1);
        switch (pname) {
            case 0x8B82:  // LINK_STATUS
                return ev::fromBool(live(c)->getProgramParameter_linkStatus(p) != GL_FALSE);
            case 0x8B80:  // DELETE_STATUS
                return ev::fromBool(false);
            default:
                // ACTIVE_UNIFORMS / ACTIVE_ATTRIBUTES / ACTIVE_UNIFORM_BLOCKS /
                // ATTACHED_SHADERS / VALIDATE_STATUS — the int path answers all
                // of them, matching the C++ context's own dispatch.
                return ev::fromDouble(live(c)->getProgramParameter_int(p, pname));
        }
    });
    b.def("getProgramInfoLog", 1, [c](Value, std::span<const Value> a) {
        return ev::fromUtf8(
            live(c)->getProgramInfoLog({idOf(argAt(a, 0), GlCell::Program)}));
    });
    b.def("isProgram", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(
            live(c)->isProgram({idOf(argAt(a, 0), GlCell::Program)}) != GL_FALSE);
    });

    // --- Locations and introspection ---
    b.def("bindAttribLocation", 3, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 2);
        if (!ev::isObject(name)) {
            live(c)->bindAttribLocation({idOf(argAt(a, 0), GlCell::Program)}, u32At(a, 1),
                                        ev::toUtf8(name));
        }
        return ev::undefined();
    });
    b.def("getAttribLocation", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (ev::isObject(name)) return ev::fromDouble(-1);
        return ev::fromDouble(
            live(c)->getAttribLocation({idOf(argAt(a, 0), GlCell::Program)}, ev::toUtf8(name)));
    });
    b.def("getFragDataLocation", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (ev::isObject(name)) return ev::fromDouble(-1);
        return ev::fromDouble(live(c)->getFragDataLocation(
            {idOf(argAt(a, 0), GlCell::Program)}, ev::toUtf8(name)));
    });
    b.def("getUniformLocation", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (ev::isObject(name)) return ev::null();
        webgl::WebGLUniformLocation loc = live(c)->getUniformLocation(
            {idOf(argAt(a, 0), GlCell::Program)}, ev::toUtf8(name));
        return wrapUniformLocation(loc);
    });
    b.def("getActiveAttrib", 2, [c](Value, std::span<const Value> a) {
        return makeActiveInfo(
            live(c)->getActiveAttrib({idOf(argAt(a, 0), GlCell::Program)}, u32At(a, 1)));
    });
    b.def("getActiveUniform", 2, [c](Value, std::span<const Value> a) {
        return makeActiveInfo(
            live(c)->getActiveUniform({idOf(argAt(a, 0), GlCell::Program)}, u32At(a, 1)));
    });
    b.def("getUniformBlockIndex", 2, [c](Value, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (ev::isObject(name)) return ev::fromDouble(4294967295.0);  // INVALID_INDEX
        return ev::fromDouble(live(c)->getUniformBlockIndex(
            {idOf(argAt(a, 0), GlCell::Program)}, ev::toUtf8(name)));
    });
    b.def("uniformBlockBinding", 3, [c](Value, std::span<const Value> a) {
        live(c)->uniformBlockBinding({idOf(argAt(a, 0), GlCell::Program)}, u32At(a, 1),
                                     u32At(a, 2));
        return ev::undefined();
    });

    // --- Scalar uniforms ---
    b.def("uniform1f", 2, [c](Value, std::span<const Value> a) {
        live(c)->uniform1f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("uniform2f", 3, [c](Value, std::span<const Value> a) {
        live(c)->uniform2f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    b.def("uniform3f", 4, [c](Value, std::span<const Value> a) {
        live(c)->uniform3f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("uniform4f", 5, [c](Value, std::span<const Value> a) {
        live(c)->uniform4f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)),
                           static_cast<float>(numAt(a, 4)));
        return ev::undefined();
    });
    b.def("uniform1i", 2, [c](Value, std::span<const Value> a) {
        live(c)->uniform1i(locOf(argAt(a, 0)), i32At(a, 1));
        return ev::undefined();
    });
    b.def("uniform2i", 3, [c](Value, std::span<const Value> a) {
        live(c)->uniform2i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("uniform3i", 4, [c](Value, std::span<const Value> a) {
        live(c)->uniform3i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("uniform4i", 5, [c](Value, std::span<const Value> a) {
        live(c)->uniform4i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                           i32At(a, 4));
        return ev::undefined();
    });
    b.def("uniform1ui", 2, [c](Value, std::span<const Value> a) {
        live(c)->uniform1ui(locOf(argAt(a, 0)), u32At(a, 1));
        return ev::undefined();
    });
    b.def("uniform2ui", 3, [c](Value, std::span<const Value> a) {
        live(c)->uniform2ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    b.def("uniform3ui", 4, [c](Value, std::span<const Value> a) {
        live(c)->uniform3ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("uniform4ui", 5, [c](Value, std::span<const Value> a) {
        live(c)->uniform4ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2), u32At(a, 3),
                            u32At(a, 4));
        return ev::undefined();
    });

    // --- Vector uniforms. One shape per element type; the component count
    //     divides the data length into the GLsizei `count` GL wants, exactly
    //     as the QuickJS layer computes it. Registration below stays a fixed
    //     def() sequence — the lambdas here are only the shared bodies. ---
    using Ctx = webgl::WebGL2RenderingContext;
    auto defFv = [&](const char* name, int comps,
                     void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLfloat*)) {
        b.def(name, 2, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<float> storage;
            const float* p = nullptr;
            size_t n = 0;
            if (floatData(argAt(a, 1), storage, &p, &n) && n >= static_cast<size_t>(comps)) {
                (live(c)->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(n / comps), p);
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
            if (int32Data(argAt(a, 1), storage, &p, &n) && n >= static_cast<size_t>(comps)) {
                (live(c)->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(n / comps), p);
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
            if (uint32Data(argAt(a, 1), storage, &p, &n) && n >= static_cast<size_t>(comps)) {
                (live(c)->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(n / comps), p);
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

    // --- Matrix uniforms: uniformMatrix{N}fv(loc, transpose, data). Square
    //     matrices only — the non-square GL 3.x forms have no three.js caller
    //     and are left out by name. ---
    auto defMat = [&](const char* name, int comps,
                      void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, GLboolean,
                                      const GLfloat*)) {
        b.def(name, 3, [c, comps, fn](Value, std::span<const Value> a) {
            std::vector<float> storage;
            const float* p = nullptr;
            size_t n = 0;
            if (floatData(argAt(a, 2), storage, &p, &n) && n >= static_cast<size_t>(comps)) {
                (live(c)->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(n / comps),
                               boolAt(a, 1) ? GL_TRUE : GL_FALSE, p);
            }
            return ev::undefined();
        });
    };
    defMat("uniformMatrix2fv", 4, &Ctx::uniformMatrix2fv);
    defMat("uniformMatrix3fv", 9, &Ctx::uniformMatrix3fv);
    defMat("uniformMatrix4fv", 16, &Ctx::uniformMatrix4fv);
}

}  // namespace bro::bronze_host

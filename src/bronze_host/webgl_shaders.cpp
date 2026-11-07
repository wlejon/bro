// Shaders, programs and uniforms for the surface three.js's program
// system drives: compile/link/introspect, uniform location lookup, scalar and
// vector uniform uploads, and the square matrix uploads WebGLUniforms leans
// on.
//
// Every call is encoded by the context's client. What a program or shader
// reports about itself is answered from what its last link or compile
// published to the client (webgl_client_async.h, webgl_program_lookup.h):
// the first such read after a link waits for that link, the rest are
// local. An object the client no longer holds (deleted, never made) is asked
// of the backend in a round trip, as is anything that is not link or
// compile state (DELETE_STATUS of a deleted object, VALIDATE_STATUS,
// ATTACHED_SHADERS, getAttachedShaders, the is* predicates).
//
// The *v upload paths read their data through floatData()/int32Data(): a
// typed array answers a borrowed bronze-heap pointer the client copies into
// the command in the next statement (nothing between them allocates); a
// plain JS array is copied into host storage first, because reading its
// elements goes through embed property reads, which allocate — the copy is
// what makes the pointer the client reads immune to that.

#include "bronze_host/webgl_internal.h"

#include "webgl/vulkan/webgl_program_lookup.h"

#include <algorithm>
#include <string>

namespace bro::bronze_host {

namespace {

using webgl::WebGLProgram;
using webgl::WebGLShader;

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

// The program's last link, or null when the client does not hold it (the
// caller asks the backend).
const webgl::ProgramLink* linkOf(webgl::WebGLClient* cl, GLuint program) { return cl->programLink(program); }

// A lookup's error goes down the ring, in order with the backend's.
void raise(webgl::WebGLClient* cl, GLenum err) {
    if (err != GL_NO_ERROR) cl->syntheticError(err);
}

// getProgramParameter of what a link leaves (and DELETE_STATUS, false for a
// program the client holds); false for a pname that is not one of those.
bool linkParameter(const webgl::ProgramLink& link, GLenum pname, GLint& out) {
    const webgl::vk::ProgramInterface& iface = *link.iface;
    switch (pname) {
        case GL_LINK_STATUS: out = link.linked ? GL_TRUE : GL_FALSE; return true;
        case GL_DELETE_STATUS: out = GL_FALSE; return true;
        case GL_ACTIVE_ATTRIBUTES: out = static_cast<GLint>(iface.attribs.size()); return true;
        case GL_ACTIVE_UNIFORMS: out = static_cast<GLint>(iface.uniforms.size()); return true;
        case GL_ACTIVE_UNIFORM_BLOCKS: out = static_cast<GLint>(iface.uniformBlocks.size()); return true;
        case GL_TRANSFORM_FEEDBACK_VARYINGS: out = static_cast<GLint>(iface.feedbackVaryings.size()); return true;
        case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: out = static_cast<GLint>(iface.feedbackBufferMode); return true;
        case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH: {
            size_t longest = 0;
            for (const auto& v : iface.feedbackVaryings) longest = std::max(longest, v.name.size() + 1);
            out = static_cast<GLint>(longest);
            return true;
        }
        default: return false;
    }
}

}  // namespace

void installWebGLShaders(ObjectBuilder& b) {
    // --- Shaders ---
    defMethod(b, "createShader", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        const GLenum type = u32At(a, 0);
        if (!cl->newShader(id, type)) return ev::null();
        return webglObject(c, WebGLCell::Shader, id, type);
    });
    defMethod(b, "deleteShader", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        if (cell) {
            const GLuint id = cell->id;
            if (auto* cl = client(c)) cl->dropShader(WebGLShader{id, cell->shaderType});
            forgetWebGLObject(c, WebGLCell::Shader, id);
        }
        return ev::undefined();
    });
    defMethod(b, "shaderSource", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        Value src = argAt(a, 1);
        if (cell && !ev::isObject(src)) {
            const WebGLShader s{cell->id, cell->shaderType};
            const std::string text = ev::toUtf8(src);
            if (auto* cl = client(c)) cl->setShaderSource(s, text);
        }
        return ev::undefined();
    });
    defMethod(b, "getShaderSource", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* cl = client(c);
        if (!cl || !cell) return ev::null();
        if (const std::string* src = cl->heldShaderSource(cell->id)) return ev::fromUtf8(*src);
        const WebGLShader s{cell->id, cell->shaderType};
        return ev::fromUtf8(cl->sync([s](WebGLBackend& gl) { return gl.getShaderSource(s); }));
    });
    defMethod(b, "compileShader", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* cl = client(c);
        if (cl && cell) cl->compileShader(WebGLShader{cell->id, cell->shaderType});
        return ev::undefined();
    });
    defMethod(b, "getShaderParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* cl = client(c);
        if (!cl || !cell) return ev::null();
        const GLenum pname = u32At(a, 1);
        const WebGLShader s{cell->id, cell->shaderType};
        switch (pname) {
            case GL_COMPILE_STATUS:
                if (const webgl::ShaderCompile* compile = cl->shaderCompile(s.id)) return ev::fromBool(compile->compiled);
                [[fallthrough]];
            case GL_DELETE_STATUS:
                if (pname == GL_DELETE_STATUS && cl->heldShaderSource(s.id)) return ev::fromBool(false);
                return ev::fromBool(cl->sync([s, pname](WebGLBackend& gl) { return gl.getShaderParameter(s, pname); }) !=
                                    GL_FALSE);
            case GL_SHADER_TYPE:
                return ev::fromDouble(cell->shaderType);
            case GL_COMPLETION_STATUS_KHR:  // with KHR_parallel_shader_compile; never waits
                if (cl->extensionEnabled("KHR_parallel_shader_compile")) {
                    if (cl->heldShaderSource(s.id)) return ev::fromBool(cl->shaderSettled(s.id));
                    if (cl->sync([s, pname](WebGLBackend& gl) { return gl.getShaderParameter(s, pname); }) == GL_TRUE)
                        return ev::fromBool(true);
                }
                cl->syntheticError(GL_INVALID_ENUM);
                return ev::null();
            default:
                cl->syntheticError(GL_INVALID_ENUM);
                return ev::null();
        }
    });
    defMethod(b, "getShaderInfoLog", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* cl = client(c);
        if (!cl || !cell) return ev::null();
        if (const webgl::ShaderCompile* compile = cl->shaderCompile(cell->id)) return ev::fromUtf8(compile->infoLog);
        const WebGLShader s{cell->id, cell->shaderType};
        return ev::fromUtf8(cl->sync([s](WebGLBackend& gl) { return gl.getShaderInfoLog(s); }));
    });
    defMethod(b, "isShader", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cell = cellOf(argAt(a, 0), WebGLCell::Shader);
        auto* cl = client(c);
        if (!cl || !cell) return ev::fromBool(false);
        const WebGLShader s{cell->id, cell->shaderType};
        return ev::fromBool(cl->sync([s](WebGLBackend& gl) { return gl.isShader(s); }) != GL_FALSE);
    });

    // --- Programs ---
    defMethod(b, "createProgram", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const GLuint id = c->newObjectId();
        cl->newProgram(id);
        return webglObject(c, WebGLCell::Program, id);
    });
    defMethod(b, "deleteProgram", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::Program);
        if (auto* cl = client(c)) cl->dropProgram(WebGLProgram{id});
        forgetWebGLObject(c, WebGLCell::Program, id);
        return ev::undefined();
    });
    defMethod(b, "attachShader", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* sh = cellOf(argAt(a, 1), WebGLCell::Shader);
        auto* cl = client(c);
        if (cl && sh)
            cl->attachShader(WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)}, WebGLShader{sh->id, sh->shaderType});
        return ev::undefined();
    });
    defMethod(b, "detachShader", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* sh = cellOf(argAt(a, 1), WebGLCell::Shader);
        auto* cl = client(c);
        if (cl && sh)
            cl->detachShader(WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)}, WebGLShader{sh->id, sh->shaderType});
        return ev::undefined();
    });
    defMethod(b, "getAttachedShaders", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        std::vector<GLuint> ids;
        std::vector<GLenum> types;
        cl->sync([&](WebGLBackend& gl) {
            ids = gl.attachedShaders(p);
            for (GLuint id : ids) types.push_back(static_cast<GLenum>(gl.getShaderParameter({id, 0}, GL_SHADER_TYPE)));
        });
        return hostArrayOf(ids.size(),
                           [&](size_t i) { return webglObject(c, WebGLCell::Shader, ids[i], types[i]); });
    });
    defMethod(b, "linkProgram", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->linkProgram(WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)});
        return ev::undefined();
    });
    defMethod(b, "validateProgram", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->validateProgram(WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)});
        return ev::undefined();
    });
    defMethod(b, "useProgram", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->useProgram(WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)});
        return ev::undefined();
    });
    defMethod(b, "getProgramParameter", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        const GLenum pname = u32At(a, 1);
        GLint value = 0;
        if (pname == GL_COMPLETION_STATUS_KHR && !cl->holdsProgram(p.id)) {
            // Not the client's (deleted, never made): the backend's answer.
        } else if (pname == GL_COMPLETION_STATUS_KHR) {
            // Never waits: KHR_parallel_shader_compile's whole point.
            if (cl->extensionEnabled("KHR_parallel_shader_compile")) return ev::fromBool(cl->programSettled(p.id));
            cl->syntheticError(GL_INVALID_ENUM);
            return ev::fromBool(false);
        }
        const webgl::ProgramLink* link =
            pname == GL_COMPLETION_STATUS_KHR || pname == GL_VALIDATE_STATUS || pname == GL_ATTACHED_SHADERS
                ? nullptr
                : cl->programLink(p.id);
        if (link) {
            if (!linkParameter(*link, pname, value)) {
                cl->syntheticError(GL_INVALID_ENUM);
                return ev::fromDouble(0);
            }
        } else {
            value = cl->sync([p, pname](WebGLBackend& gl) { return gl.getProgramParameter(p, pname); });
        }
        switch (pname) {
            case GL_LINK_STATUS:
            case GL_DELETE_STATUS:
            case GL_VALIDATE_STATUS:
            case GL_COMPLETION_STATUS_KHR:
                return ev::fromBool(value != 0);
            default:
                return ev::fromDouble(value);
        }
    });
    defMethod(b, "getProgramInfoLog", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id)) return ev::fromUtf8(link->infoLog);
        return ev::fromUtf8(cl->sync([p](WebGLBackend& gl) { return gl.getProgramInfoLog(p); }));
    });
    defMethod(b, "isProgram", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        return ev::fromBool(cl && cl->sync([p](WebGLBackend& gl) { return gl.isProgram(p); }) != GL_FALSE);
    });

    // --- Locations and introspection: from the program's last link ---
    defMethod(b, "bindAttribLocation", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        Value name = argAt(a, 2);
        if (ev::isObject(name)) return ev::undefined();
        const std::string n = ev::toUtf8(name);
        if (auto* cl = client(c))
            cl->bindAttribLocation(WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1), n);
        return ev::undefined();
    });
    defMethod(b, "getAttribLocation", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (!client(c) || ev::isObject(name)) return ev::fromDouble(-1);
        const std::string n = ev::toUtf8(name);
        auto* cl = client(c);  // converting the name may have run script that lost the context
        if (!cl) return ev::fromDouble(-1);
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id)) {
            if (!link->linked) {
                cl->syntheticError(GL_INVALID_OPERATION);
                return ev::fromDouble(-1);
            }
            return ev::fromDouble(webgl::vk::lookupAttribLocation(*link->iface, n));
        }
        return ev::fromDouble(cl->sync([&](WebGLBackend& gl) { return gl.getAttribLocation(p, n); }));
    });
    defMethod(b, "getFragDataLocation", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (!client(c) || ev::isObject(name)) return ev::fromDouble(-1);
        const std::string n = ev::toUtf8(name);
        auto* cl = client(c);
        if (!cl) return ev::fromDouble(-1);
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id)) {
            if (!link->linked) {
                cl->syntheticError(GL_INVALID_OPERATION);
                return ev::fromDouble(-1);
            }
            return ev::fromDouble(webgl::vk::lookupFragDataLocation(*link->iface, n));
        }
        return ev::fromDouble(cl->sync([&](WebGLBackend& gl) { return gl.getFragDataLocation(p, n); }));
    });
    defMethod(b, "getUniformLocation", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (!client(c) || ev::isObject(name)) return ev::null();
        const std::string n = ev::toUtf8(name);
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id)) {
            if (!link->linked) {
                cl->syntheticError(GL_INVALID_OPERATION);
                return ev::null();
            }
            return wrapUniformLocation({webgl::vk::lookupUniformLocation(*link->iface, n), p.id});
        }
        return wrapUniformLocation(cl->sync([&](WebGLBackend& gl) { return gl.getUniformLocation(p, n); }));
    });
    defMethod(b, "getActiveAttrib", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        const GLuint index = u32At(a, 1);
        webgl::WebGLActiveInfo info{"", 0, 0};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id))
            raise(cl, webgl::vk::lookupActiveAttrib(*link->iface, index, info));
        else
            info = cl->sync([&](WebGLBackend& gl) { return gl.getActiveAttrib(p, index); });
        return makeActiveInfo(info);
    });
    defMethod(b, "getActiveUniform", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        const GLuint index = u32At(a, 1);
        webgl::WebGLActiveInfo info{"", 0, 0};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id))
            raise(cl, webgl::vk::lookupActiveUniform(*link->iface, index, info));
        else
            info = cl->sync([&](WebGLBackend& gl) { return gl.getActiveUniform(p, index); });
        return makeActiveInfo(info);
    });
    defMethod(b, "getUniformBlockIndex", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        Value name = argAt(a, 1);
        if (!client(c) || ev::isObject(name)) return ev::fromDouble(4294967295.0);  // INVALID_INDEX
        const std::string n = ev::toUtf8(name);
        auto* cl = client(c);
        if (!cl) return ev::fromDouble(4294967295.0);
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        if (const webgl::ProgramLink* link = linkOf(cl, p.id))
            return ev::fromDouble(webgl::vk::lookupUniformBlockIndex(*link->iface, n));
        return ev::fromDouble(cl->sync([&](WebGLBackend& gl) { return gl.getUniformBlockIndex(p, n); }));
    });
    defMethod(b, "getActiveUniformBlockName", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram p{idOf(argAt(a, 0), WebGLCell::Program)};
        const GLuint index = u32At(a, 1);
        std::string name;
        if (const webgl::ProgramLink* link = linkOf(cl, p.id))
            raise(cl, webgl::vk::lookupUniformBlockName(*link->iface, index, name));
        else
            name = cl->sync([&](WebGLBackend& gl) { return gl.getActiveUniformBlockName(p, index); });
        return ev::fromUtf8(name);
    });
    defMethod(b, "getUniformIndices", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const WebGLProgram prog{idOf(argAt(a, 0), WebGLCell::Program)};
        Value namesVal = argAt(a, 1);
        if (!client(c) || !ev::isObject(namesVal)) return ev::null();
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
        auto* cl = client(c);
        if (!cl) return ev::null();
        std::vector<GLuint> indices;
        if (const webgl::ProgramLink* link = linkOf(cl, prog.id))
            indices = webgl::vk::lookupUniformIndices(*link->iface, names);
        else
            indices = cl->sync([&](WebGLBackend& gl) { return gl.getUniformIndices(prog, names); });
        return hostArrayOf(indices.size(), [&indices](size_t i) { return ev::fromDouble(indices[i]); });
    });
    defMethod(b, "getActiveUniforms", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const WebGLProgram prog{idOf(argAt(a, 0), WebGLCell::Program)};
        uint32_t pname = u32At(a, 2);
        SmallBuffer<uint32_t, 64> storage;
        const uint32_t* data = nullptr;
        size_t count = 0;
        if (!uint32Data(argAt(a, 1), storage, &data, &count)) return ev::null();
        auto* cl = client(c);
        if (!cl) return ev::null();
        const std::vector<GLuint> indices(data, data + count);
        std::vector<GLint> params;
        if (const webgl::ProgramLink* link = linkOf(cl, prog.id))
            raise(cl, webgl::vk::lookupActiveUniforms(*link->iface, indices, pname, params));
        else
            params = cl->sync([&](WebGLBackend& gl) { return gl.getActiveUniforms(prog, indices, pname); });
        return hostArrayOf(params.size(), [&params, pname](size_t i) {
            if (pname == 0x8A3E /* UNIFORM_IS_ROW_MAJOR */) {
                return ev::fromBool(params[i] != 0);
            }
            return ev::fromDouble(params[i]);
        });
    });
    defMethod(b, "getActiveUniformBlockParameter", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        if (!cl) return ev::null();
        const WebGLProgram prog{idOf(argAt(a, 0), WebGLCell::Program)};
        uint32_t blockIndex = u32At(a, 1);
        uint32_t pname = u32At(a, 2);
        // UNIFORM_BLOCK_BINDING is program state (uniformBlockBinding), not
        // what the link left: the backend's.
        const webgl::ProgramLink* link = pname == GL_UNIFORM_BLOCK_BINDING ? nullptr : linkOf(cl, prog.id);
        switch (pname) {
            case 0x8A43: {  // UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES
                std::vector<GLint> indices =
                    link ? webgl::vk::lookupUniformBlockIndices(*link->iface, blockIndex)
                         : cl->sync([&](WebGLBackend& gl) { return gl.getActiveUniformBlockIndices(prog, blockIndex); });
                return hostArrayOf(indices.size(), [&indices](size_t i) {
                    return ev::fromDouble(static_cast<uint32_t>(indices[i]));
                });
            }
            default: {
                GLint value = 0;
                if (link)
                    raise(cl, webgl::vk::lookupUniformBlockParameter(*link->iface, blockIndex, pname, value));
                else
                    value = cl->sync(
                        [&](WebGLBackend& gl) { return gl.getActiveUniformBlockParameteri(prog, blockIndex, pname); });
                if (pname == 0x8A44 /* UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER */ ||
                    pname == 0x8A46 /* UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER */)
                    return ev::fromBool(value != 0);
                return ev::fromDouble(value);
            }
        }
    });
    defMethod(b, "uniformBlockBinding", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->uniformBlockBinding(webgl::WebGLProgram{idOf(argAt(a, 0), WebGLCell::Program)}, u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });

    // --- Scalar uniforms ---
    defMethod(b, "uniform1f", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform1f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    defMethod(b, "uniform2f", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->uniform2f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                          static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });
    defMethod(b, "uniform3f", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->uniform3f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    defMethod(b, "uniform4f", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->uniform4f(locOf(argAt(a, 0)), static_cast<float>(numAt(a, 1)),
                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)),
                          static_cast<float>(numAt(a, 4)));
        return ev::undefined();
    });
    defMethod(b, "uniform1i", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform1i(locOf(argAt(a, 0)), i32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "uniform2i", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform2i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "uniform3i", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform3i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "uniform4i", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->uniform4i(locOf(argAt(a, 0)), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                          i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "uniform1ui", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform1ui(locOf(argAt(a, 0)), u32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "uniform2ui", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform2ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "uniform3ui", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->uniform3ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "uniform4ui", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->uniform4ui(locOf(argAt(a, 0)), u32At(a, 1), u32At(a, 2), u32At(a, 3),
                           u32At(a, 4));
        return ev::undefined();
    });

    // --- Vector uniforms. One shape per element type; the component count
    //     divides the data length into the uniform `count`.
    //     Registration below stays a fixed def() sequence. ---
    using Ctx = webgl::WebGLClient;
    auto defFv = [&](const char* name, int comps,
                     void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLfloat*, size_t)) {
        defMethod(b, name, 2, [comps, fn](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
            SmallBuffer<float, 64> storage;
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
                auto* cl = client(c);
                const size_t uniforms = count / static_cast<size_t>(comps);
                if (cl && uniforms > 0)
                    (cl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(uniforms), p + srcOffset,
                              uniforms * static_cast<size_t>(comps));
            }
            return ev::undefined();
        });
    };
    auto defIv = [&](const char* name, int comps,
                     void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLint*, size_t)) {
        defMethod(b, name, 2, [comps, fn](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
            SmallBuffer<int32_t, 64> storage;
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
                auto* cl = client(c);
                const size_t uniforms = count / static_cast<size_t>(comps);
                if (cl && uniforms > 0)
                    (cl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(uniforms), p + srcOffset,
                              uniforms * static_cast<size_t>(comps));
            }
            return ev::undefined();
        });
    };
    auto defUiv = [&](const char* name, int comps,
                      void (Ctx::*fn)(webgl::WebGLUniformLocation, GLsizei, const GLuint*, size_t)) {
        defMethod(b, name, 2, [comps, fn](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
            SmallBuffer<uint32_t, 64> storage;
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
                auto* cl = client(c);
                const size_t uniforms = count / static_cast<size_t>(comps);
                if (cl && uniforms > 0)
                    (cl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(uniforms), p + srcOffset,
                              uniforms * static_cast<size_t>(comps));
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
                                      const GLfloat*, size_t)) {
        defMethod(b, name, 3, [comps, fn](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
            SmallBuffer<float, 64> storage;
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
                auto* cl = client(c);
                const size_t uniforms = count / static_cast<size_t>(comps);
                if (cl && uniforms > 0)
                    (cl->*fn)(locOf(argAt(a, 0)), static_cast<GLsizei>(uniforms),
                              static_cast<GLboolean>(boolAt(a, 1) ? GL_TRUE : GL_FALSE), p + srcOffset,
                              uniforms * static_cast<size_t>(comps));
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

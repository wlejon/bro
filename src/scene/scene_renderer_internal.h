#pragma once

// Shared GL helpers for the SceneRenderer translation units. SceneRenderer's
// implementation is split across scene_renderer*.cpp (mesh, instanced, shadow,
// post-FX, environment, overlays, lighting, core); these small helpers are
// used by all of them, so they live here as inline functions rather than a
// file-local static in any one unit.

#include <glad/gl.h>

#include <cassert>
#include <cstring>
#include <string>

#include "util/log.h"
#include "scene/depth_policy.h"

namespace bro::scene {

// Insert `line` (must be newline-terminated) right after the source's
// #version line — GLSL requires #version to stay the first directive, so a
// plain prepend won't do. NOTE: keeps the NVIDIA gotcha in mind — the shader
// sources keep #version literally on line 1 and never mention the directive
// inside comments, so the first find() hit is the real directive.
inline std::string insertAfterVersion(std::string s, const std::string& line) {
    size_t v = s.find("#version");
    if (v == std::string::npos) return line + s;
    size_t nl = s.find('\n', v);
    if (nl == std::string::npos) return s + "\n" + line;
    s.insert(nl + 1, line);
    return s;
}

// Build the skinned variants of mesh.vert / shadow.vert from the same
// embedded source.
inline std::string withSkinnedDefine(const char* src) {
    return insertAfterVersion(src ? src : "", "#define SKINNED 1\n");
}

// Build the instanced fragment shader from the regular mesh fragment source
// (instance tint varying, atlas UV remap). Defined in
// scene_renderer_instanced.cpp; also used by the custom-shader path to
// splice user fragment chunks into the instanced variant.
std::string makeMeshInstancedFragSrc();

// Build a custom-shader variant of mesh.vert / mesh.frag: inject
// `#define <defineName> 1` after the #version line (activating the
// userVertex/userFragment call in main) and replace the `//__USER_CHUNK__`
// marker line with the user's GLSL chunk. Empty chunk returns the source
// unchanged (the marker stays an inert comment).
inline std::string withUserChunk(const char* src, const std::string& chunk,
                                 const char* defineName) {
    std::string s(src ? src : "");
    if (chunk.empty()) return s;
    s = insertAfterVersion(std::move(s),
                           std::string("#define ") + defineName + " 1\n");
    const char* marker = "//__USER_CHUNK__";
    size_t m = s.find(marker);
    if (m != std::string::npos) {
        s.replace(m, std::strlen(marker), "\n" + chunk + "\n");
    } else {
        // A missing marker means the shader source was edited without
        // keeping the splice point — the user's chunk would be silently
        // dropped. Make it loud (and fatal in Debug).
        LOG_ERROR("withUserChunk: \"%s\" marker not found — user %s chunk "
                  "dropped (shader source edited without the marker?)",
                  marker, defineName);
        assert(!"withUserChunk: __USER_CHUNK__ marker missing");
    }
    return s;
}

// Compile a single shader stage. Returns 0 (and logs) on failure.
inline GLuint compileShader(GLenum type, const char* src) {
    // Every program in the renderer funnels through here, so the depth
    // convention is injected once rather than at each call site — a shader
    // that encodes the convention (the skybox's far-plane z, the depth
    // linearizers, the depth->world reconstructions) can never be missed as
    // the renderer grows. Shaders that don't care just ignore the define.
    const std::string withPolicy =
        gReversedZ ? insertAfterVersion(src ? src : "", "#define REVERSED_Z 1\n")
                   : std::string(src ? src : "");
    const char* finalSrc = withPolicy.c_str();

    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &finalSrc, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR("Mesh shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Compile a vertex+fragment pair and link them into a program. Returns 0 (and
// logs "<label> link error") on any compile or link failure. `label` is the
// pipeline name used in the diagnostic. The shader objects are always deleted.
inline GLuint linkProgram(const char* vsSrc, const char* fsSrc, const char* label) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_ERROR("%s link error: %s", label, log);
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// linkProgram variant that captures the FULL driver info log into *errOut on
// any compile/link failure (the plain helpers truncate to 512 bytes and only
// LOG_ERROR). Used by the custom-shader path so GLSL errors can be surfaced
// verbatim to JS as a thrown exception.
inline GLuint linkProgramCapture(const char* vsSrc, const char* fsSrc,
                                 std::string* errOut) {
    auto compile = [&](GLenum type, const char* src,
                       const char* stage) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::string log(len > 1 ? len - 1 : 0, '\0');
            if (len > 1) glGetShaderInfoLog(s, len, nullptr, log.data());
            if (errOut) *errOut = std::string(stage) + " compile error:\n" + log;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc, "vertex shader");
    if (!vs) return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc, "fragment shader");
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 1 ? len - 1 : 0, '\0');
        if (len > 1) glGetProgramInfoLog(prog, len, nullptr, log.data());
        if (errOut) *errOut = "program link error:\n" + log;
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

}  // namespace bro::scene

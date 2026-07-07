#pragma once

// Shared GL helpers for the SceneRenderer translation units. SceneRenderer's
// implementation is split across scene_renderer*.cpp (mesh, instanced, shadow,
// post-FX, environment, overlays, lighting, core); these small helpers are
// used by all of them, so they live here as inline functions rather than a
// file-local static in any one unit.

#include <glad/gl.h>

#include "util/log.h"

namespace bro::scene {

// Compile a single shader stage. Returns 0 (and logs) on failure.
inline GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
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

}  // namespace bro::scene

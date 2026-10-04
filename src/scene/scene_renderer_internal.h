#pragma once

// Shared GL helpers for the SceneRenderer translation units. SceneRenderer's
// implementation is split across scene_renderer*.cpp (mesh, instanced, shadow,
// post-FX, environment, overlays, lighting, core); these small helpers are
// used by all of them, so they live here as inline functions rather than a
// file-local static in any one unit.

#include "atmosphere.glsl.h"   // kAtmosphereSrc
#include "webgl/webgl_types.h"

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

// Build a custom-shader variant of mesh.vert / mesh.frag: inject
// `#define <defineName> 1` after the #version line (activating the
// userVertex/userFragment call in main) and replace the `//__USER_CHUNK__`
// marker line with the user's GLSL chunk. Empty chunk returns the source
// unchanged (the marker stays an inert comment).
// The atmosphere model, spliced into a mesh fragment shader so aerial
// perspective integrates exactly what the sky pass integrates. Always injected
// rather than made a program variant: the atmosphere can be toggled at runtime,
// and a variant key would double the mesh program matrix for a branch the GPU
// takes uniformly. uAtmEnabled gates it at zero cost when it is off.
inline std::string withAtmosphere(const char* src) {
    return insertAfterVersion(src ? src : "", std::string(kAtmosphereSrc) + "\n");
}

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
        LOG_ERROR("withUserChunk: \"%s\" marker not found — user %s chunk "
                  "dropped (shader source edited without the marker?)",
                  marker, defineName);
        assert(!"withUserChunk: __USER_CHUNK__ marker missing");
    }
    return s;
}

inline GLuint compileShader(GLenum /*type*/, const char* /*src*/) {
    return 0;
}

inline GLuint linkProgram(const char* /*vsSrc*/, const char* /*fsSrc*/, const char* /*label*/) {
    return 0;
}

inline GLuint linkProgramCapture(const char* /*vsSrc*/, const char* /*fsSrc*/,
                                 std::string* /*errOut*/) {
    return 0;
}

}  // namespace bro::scene

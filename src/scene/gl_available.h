#pragma once

#include "render/gl_compat.h"

namespace bro::scene {

using ::GLuint;
using ::GLint;
using ::GLenum;
using ::GLsizei;
using ::GLfloat;
using ::GLboolean;
using ::GLclampd;
using ::GLsizeiptr;

/// Desktop OpenGL has been purged across bro in favor of Vulkan.
/// Legacy GL function pointers are never loaded.
inline bool glFunctionsLoaded() {
    return false;
}

}  // namespace bro::scene

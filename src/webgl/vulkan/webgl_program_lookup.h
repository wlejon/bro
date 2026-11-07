#pragma once

// What a linked program reports about itself, answered from its interface
// alone (ProgramInterface: glslang's reflection, fixed at link time): plain
// functions of that data, with no context, so whichever thread holds a
// program's published interface answers the same as the backend does.
//
// Each returns the GL error the query raises (GL_NO_ERROR for none) where
// one can; the caller records it. Whether the program exists and has linked
// is the caller's to check first.

#include "webgl/vulkan/webgl_vk_types.h"

#include <string>
#include <vector>

namespace bro::webgl::vk {

/// getAttribLocation: the attribute's location, -1 for none.
GLint lookupAttribLocation(const ProgramInterface& iface, const std::string& name);

/// getFragDataLocation: "out[k]" names element k of an output array.
GLint lookupFragDataLocation(const ProgramInterface& iface, const std::string& name);

/// getUniformLocation: "u" and "u[0]" name an array's first element, "u[k]"
/// its k-th; block members have no location. -1 for none.
GLint lookupUniformLocation(const ProgramInterface& iface, const std::string& name);

/// getUniformBlockIndex: an array of blocks is named "Block[0]", and "Block"
/// is its first element. GL_INVALID_INDEX for none.
GLuint lookupUniformBlockIndex(const ProgramInterface& iface, const std::string& name);

/// getActiveAttrib / getActiveUniform: INVALID_VALUE for an index past the end.
GLenum lookupActiveAttrib(const ProgramInterface& iface, GLuint index, WebGLActiveInfo& out);
GLenum lookupActiveUniform(const ProgramInterface& iface, GLuint index, WebGLActiveInfo& out);

/// getUniformIndices: GL_INVALID_INDEX for a name that is not an active uniform.
std::vector<GLuint> lookupUniformIndices(const ProgramInterface& iface, const std::vector<std::string>& names);

/// getActiveUniforms: `pname` of each uniform in `indices`. INVALID_VALUE
/// for an index past the end, INVALID_ENUM for a pname it does not know
/// (`out` empty either way).
GLenum lookupActiveUniforms(const ProgramInterface& iface, const std::vector<GLuint>& indices, GLenum pname,
                            std::vector<GLint>& out);

/// getActiveUniformBlockParameter of what the interface holds (every pname
/// but UNIFORM_BLOCK_BINDING, which is program state and changes after the
/// link): INVALID_VALUE for an index past the end, INVALID_ENUM for an
/// unknown pname.
GLenum lookupUniformBlockParameter(const ProgramInterface& iface, GLuint blockIndex, GLenum pname, GLint& out);

/// UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES; empty for an index past the end.
std::vector<GLint> lookupUniformBlockIndices(const ProgramInterface& iface, GLuint blockIndex);

/// getActiveUniformBlockName: INVALID_VALUE for an index past the end.
GLenum lookupUniformBlockName(const ProgramInterface& iface, GLuint blockIndex, std::string& out);

} // namespace bro::webgl::vk

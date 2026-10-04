#pragma once

#include "webgl/webgl_objects.h"
#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
#include "webgl/webgl_types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <memory>

namespace bro::webgl::vk {

/// Descriptor bindings every translated program uses: samplers from 0, uniform
/// blocks from kFirstUniformBlockBinding, and the default-block (non-block)
/// uniforms as one std140 uniform buffer at kDefaultUniformBinding.
constexpr uint32_t kMaxSamplerBindings = 8;
constexpr uint32_t kFirstUniformBlockBinding = 8;
constexpr uint32_t kMaxUniformBlockBindings = 8;
constexpr uint32_t kDefaultUniformBinding = 16;

/// Buffer object: a device-local VkBuffer usable for every GL binding target
/// (WebGL lets one buffer be rebound to any target but ELEMENT_ARRAY), written
/// only by copies recorded in the context's command stream, plus the
/// authoritative host-side copy that reads (getBufferSubData, 8-bit indices,
/// PBO sources) are served from.
struct VkBufferResource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize offset = 0;
    uint64_t allocId = 0;
    std::vector<uint8_t> shadowData;
    bool isMapped = false;
    void* mappedPtr = nullptr;

    bool isValid() const { return buffer != VK_NULL_HANDLE; }
};
using WebGLBufferResource = VkBufferResource;

/// 2D Texture object backed by Vulkan image, view, and sampler.
struct VkTextureResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint64_t allocId = 0;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView attachmentView = VK_NULL_HANDLE;  // mip 0 / layer 0, for framebuffer use
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // of every mip and layer
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;

    GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR;
    GLenum magFilter = GL_LINEAR;
    GLenum wrapS = GL_REPEAT;
    GLenum wrapT = GL_REPEAT;
    GLenum target = 0x0DE1 /* GL_TEXTURE_2D */;
    uint32_t depth = 1;
    bool samplerDirty = true;
    uint32_t bytesPerPixel = 4;

    bool isValid() const { return image != VK_NULL_HANDLE; }
};
using WebGLTextureResource = VkTextureResource;

/// Sampler object backed by Vulkan sampler.
struct VkSamplerResource {
    VkSampler sampler = VK_NULL_HANDLE;
    GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR;
    GLenum magFilter = GL_LINEAR;
    GLenum wrapS = GL_REPEAT;
    GLenum wrapT = GL_REPEAT;
    GLenum wrapR = GL_REPEAT;
    GLfloat minLod = -1000.0f;
    GLfloat maxLod = 1000.0f;
    GLenum compareMode = GL_NONE;
    GLenum compareFunc = GL_LEQUAL;
    bool samplerDirty = true;
};

/// Attribute metadata tracked in a linked program.
struct VkAttribInfo {
    std::string name;
    GLenum type = GL_FLOAT_VEC4;
    GLint size = 1;
    GLint location = -1;
};

/// Vertex attribute specification within a Vertex Array Object (VAO).
struct VkVertexAttribute {
    bool enabled = false;
    GLint size = 4; // 1, 2, 3, 4
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    bool isInteger = false;
    GLsizei stride = 0;
    uintptr_t offset = 0;
    GLuint bufferId = 0;
    GLuint divisor = 0;
};

/// Vertex Array Object (VAO) state.
struct VkVAOResource {
    std::array<VkVertexAttribute, 16> attributes{};
    GLuint elementArrayBufferId = 0;
};

/// WebGL Shader compiled to SPIR-V.
struct VkShaderResource {
    GLenum type = 0; // GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
    std::string source;
    std::string translatedSource;
    std::vector<uint32_t> spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    bool compileStatus = false;
    std::string infoLog;

    uint32_t attachCount = 0;
    bool deleteStatus = false;

    bool isValid() const { return module != VK_NULL_HANDLE; }
};

/// Uniform metadata tracked in a linked program.
struct VkUniformInfo {
    std::string name;
    GLint location = -1;
    GLenum type = GL_FLOAT;
    uint32_t offset = 0; // byte offset in push constants / uniform block
    uint32_t size = 0;   // byte size
    GLint count = 1;     // array count
    GLint blockIndex = -1; // -1 if default/push_constant, >=0 if UBO member
    GLint matrixStride = 0;
    GLint arrayStride = 0;
    GLboolean isRowMajor = GL_FALSE;
};

/// Uniform block metadata tracked in a linked program.
struct VkUniformBlockInfo {
    std::string name;
    GLuint index = 0;
    GLuint binding = 0;
    GLuint descriptorBinding = 8;
    uint32_t dataSize = 0;
    std::vector<GLuint> activeUniformIndices;
    bool referencedByVertex = false;
    bool referencedByFragment = false;
};

/// WebGL Program linking vertex and fragment shaders.
struct VkProgramResource {
    GLuint vertShaderId = 0;
    GLuint fragShaderId = 0;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    bool linkStatus = false;
    bool deleteStatus = false;
    std::string infoLog;

    std::vector<VkUniformInfo> uniforms;
    std::unordered_map<std::string, GLint> uniformLocations;
    std::vector<uint8_t> uniformBytes;

    std::vector<VkAttribInfo> activeAttribs;
    std::unordered_map<std::string, GLint> attribLocations;
    std::unordered_map<std::string, GLuint> boundAttribLocations;
    std::unordered_map<std::string, GLint> fragDataLocations;
    std::vector<VkUniformBlockInfo> uniformBlocks;
    std::unordered_map<std::string, GLuint> uniformBlockIndices;
    std::unordered_map<GLuint, GLuint> uniformBlockBindings;
    std::unordered_map<GLint, uint32_t> samplerLocToBinding;
    std::unordered_map<uint32_t, uint32_t> samplerBindings; // descriptor binding -> texture unit
    std::unordered_map<uint32_t, GLenum> samplerTypes; // descriptor binding -> GL_SAMPLER_2D, GL_SAMPLER_CUBE, etc.

    // What the last draw bound, reused by the next draw in the same frame when
    // nothing changed (webgl_vk_context_draw.cpp). Descriptor sets and the
    // uniform slice come from the frame's arenas, so they never outlive it.
    uint64_t drawFrameSerial = 0;
    std::vector<uint8_t> drawUniformBytes;
    VkDescriptorBufferInfo drawUniforms{};
    std::vector<uint64_t> drawBindingKey;
    VkDescriptorSet drawSet = VK_NULL_HANDLE;

    bool isValid() const { return linkStatus; }
};

/// Framebuffer object.
struct VkFramebufferResource {
    std::array<GLuint, 8> colorAttachments{};
    GLuint colorAttachmentTex = 0;
    GLuint depthAttachmentTex = 0;
    std::vector<GLenum> drawBuffers{0x8CE0 /* GL_COLOR_ATTACHMENT0 */};
    GLenum readBuffer = 0x8CE0;
    bool isComplete = true;
};

/// Renderbuffer object.
struct VkRenderbufferResource {
    GLuint textureId = 0;
    GLenum internalformat = 0;
    GLsizei width = 0;
    GLsizei height = 0;
    GLsizei samples = 0;
    bool isValid() const { return textureId != 0; }
};

// ---------------------------------------------------------------------------
// Format and State Translation Helpers
// ---------------------------------------------------------------------------

inline VkFormat glTypeToVkFormat(GLenum type, GLint size, GLboolean normalized, bool isInteger = false) {
    if (isInteger || type == GL_INT || type == GL_UNSIGNED_INT) {
        if (type == GL_INT) {
            switch (size) {
                case 1: return VK_FORMAT_R32_SINT;
                case 2: return VK_FORMAT_R32G32_SINT;
                case 3: return VK_FORMAT_R32G32B32_SINT;
                case 4: return VK_FORMAT_R32G32B32A32_SINT;
            }
        } else if (type == GL_UNSIGNED_INT) {
            switch (size) {
                case 1: return VK_FORMAT_R32_UINT;
                case 2: return VK_FORMAT_R32G32_UINT;
                case 3: return VK_FORMAT_R32G32B32_UINT;
                case 4: return VK_FORMAT_R32G32B32A32_UINT;
            }
        } else if (type == GL_SHORT) {
            switch (size) {
                case 1: return VK_FORMAT_R16_SINT;
                case 2: return VK_FORMAT_R16G16_SINT;
                case 3: return VK_FORMAT_R16G16B16_SINT;
                case 4: return VK_FORMAT_R16G16B16A16_SINT;
            }
        } else if (type == GL_UNSIGNED_SHORT) {
            switch (size) {
                case 1: return VK_FORMAT_R16_UINT;
                case 2: return VK_FORMAT_R16G16_UINT;
                case 3: return VK_FORMAT_R16G16B16_UINT;
                case 4: return VK_FORMAT_R16G16B16A16_UINT;
            }
        } else if (type == GL_BYTE) {
            switch (size) {
                case 1: return VK_FORMAT_R8_SINT;
                case 2: return VK_FORMAT_R8G8_SINT;
                case 3: return VK_FORMAT_R8G8B8_SINT;
                case 4: return VK_FORMAT_R8G8B8A8_SINT;
            }
        } else if (type == GL_UNSIGNED_BYTE) {
            switch (size) {
                case 1: return VK_FORMAT_R8_UINT;
                case 2: return VK_FORMAT_R8G8_UINT;
                case 3: return VK_FORMAT_R8G8B8_UINT;
                case 4: return VK_FORMAT_R8G8B8A8_UINT;
            }
        }
    }
    if (type == GL_FLOAT) {
        switch (size) {
            case 1: return VK_FORMAT_R32_SFLOAT;
            case 2: return VK_FORMAT_R32G32_SFLOAT;
            case 3: return VK_FORMAT_R32G32B32_SFLOAT;
            case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
            default: return VK_FORMAT_R32G32B32A32_SFLOAT;
        }
    } else if (type == GL_UNSIGNED_BYTE) {
        if (normalized) {
            switch (size) {
                case 1: return VK_FORMAT_R8_UNORM;
                case 2: return VK_FORMAT_R8G8_UNORM;
                case 3: return VK_FORMAT_R8G8B8_UNORM;
                case 4: return VK_FORMAT_R8G8B8A8_UNORM;
            }
        } else {
            switch (size) {
                case 1: return VK_FORMAT_R8_UINT;
                case 2: return VK_FORMAT_R8G8_UINT;
                case 3: return VK_FORMAT_R8G8B8_UINT;
                case 4: return VK_FORMAT_R8G8B8A8_UINT;
            }
        }
    } else if (type == GL_BYTE) {
        if (normalized) {
            switch (size) {
                case 1: return VK_FORMAT_R8_SNORM;
                case 2: return VK_FORMAT_R8G8_SNORM;
                case 3: return VK_FORMAT_R8G8B8_SNORM;
                case 4: return VK_FORMAT_R8G8B8A8_SNORM;
            }
        } else {
            switch (size) {
                case 1: return VK_FORMAT_R8_SINT;
                case 2: return VK_FORMAT_R8G8_SINT;
                case 3: return VK_FORMAT_R8G8B8_SINT;
                case 4: return VK_FORMAT_R8G8B8A8_SINT;
            }
        }
    } else if (type == GL_UNSIGNED_SHORT) {
        if (normalized) {
            switch (size) {
                case 1: return VK_FORMAT_R16_UNORM;
                case 2: return VK_FORMAT_R16G16_UNORM;
                case 3: return VK_FORMAT_R16G16B16_UNORM;
                case 4: return VK_FORMAT_R16G16B16A16_UNORM;
            }
        } else {
            switch (size) {
                case 1: return VK_FORMAT_R16_UINT;
                case 2: return VK_FORMAT_R16G16_UINT;
                case 3: return VK_FORMAT_R16G16B16_UINT;
                case 4: return VK_FORMAT_R16G16B16A16_UINT;
            }
        }
    } else if (type == GL_INT) {
        switch (size) {
            case 1: return VK_FORMAT_R32_SINT;
            case 2: return VK_FORMAT_R32G32_SINT;
            case 3: return VK_FORMAT_R32G32B32_SINT;
            case 4: return VK_FORMAT_R32G32B32A32_SINT;
        }
    } else if (type == GL_UNSIGNED_INT) {
        switch (size) {
            case 1: return VK_FORMAT_R32_UINT;
            case 2: return VK_FORMAT_R32G32_UINT;
            case 3: return VK_FORMAT_R32G32B32_UINT;
            case 4: return VK_FORMAT_R32G32B32A32_UINT;
        }
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

inline VkPrimitiveTopology glTopologyToVk(GLenum mode) {
    switch (mode) {
        case GL_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_LINE_LOOP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

inline VkBlendFactor glBlendFactorToVk(GLenum factor) {
    switch (factor) {
        case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
        case GL_ONE: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

inline VkBlendOp glBlendOpToVk(GLenum op) {
    switch (op) {
        case GL_FUNC_ADD: return VK_BLEND_OP_ADD;
        case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case GL_MIN: return VK_BLEND_OP_MIN;
        case GL_MAX: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

inline VkCompareOp glCompareOpToVk(GLenum func) {
    switch (func) {
        case GL_NEVER: return VK_COMPARE_OP_NEVER;
        case GL_LESS: return VK_COMPARE_OP_LESS;
        case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
        case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case GL_GREATER: return VK_COMPARE_OP_GREATER;
        case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case GL_ALWAYS: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_LESS;
    }
}

inline VkStencilOp glStencilOpToVk(GLenum op) {
    switch (op) {
        case GL_KEEP: return VK_STENCIL_OP_KEEP;
        case GL_ZERO: return VK_STENCIL_OP_ZERO;
        case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
        case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        case GL_INVERT: return VK_STENCIL_OP_INVERT;
        default: return VK_STENCIL_OP_KEEP;
    }
}

inline VkCullModeFlags glCullModeToVk(GLenum mode) {
    switch (mode) {
        case GL_FRONT: return VK_CULL_MODE_FRONT_BIT;
        case GL_BACK: return VK_CULL_MODE_BACK_BIT;
        case GL_FRONT_AND_BACK: return VK_CULL_MODE_FRONT_AND_BACK;
        default: return VK_CULL_MODE_BACK_BIT;
    }
}

inline VkFrontFace glFrontFaceToVk(GLenum mode) {
    switch (mode) {
        case GL_CW: return VK_FRONT_FACE_CLOCKWISE;
        case GL_CCW: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
        default: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
}

} // namespace bro::webgl::vk

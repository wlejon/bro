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

/// Where GL's window y runs on the bound render target. The canvas is drawn
/// top-down for presentation (a negative viewport maps GL's bottom-up NDC onto
/// it), so its window y counts down from the bottom row; a framebuffer object's
/// texture keeps GL's own order (row 0 is window y 0, as texImage2D and
/// readPixels have it), so it is drawn unflipped. The fragment stage gets the
/// mapping as a push constant and rebuilds GL's gl_FragCoord.y
/// (yOffset + yScale * y), dFdy (yScale * dFdy) and gl_PointCoord from
/// Vulkan's.
struct FragmentPush {
    float yOffset = 0.0f;
    float yScale = 1.0f;
};

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
    // Single-level, single-layer views a framebuffer renders through, keyed
    // by (level << 16 | layer); made on first use.
    std::vector<std::pair<uint32_t, VkImageView>> attachmentViews;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // of every mip and layer
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;  // > 1 only for multisampled renderbuffers

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

/// A shader object: its source and the result of the last compileShader.
/// Compiling only checks the source (with glslang, for the info log); the
/// SPIR-V is produced when a program links, from both stages together.
struct VkShaderResource {
    GLenum type = 0; // GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
    std::string source;
    bool compileStatus = false;
    std::string infoLog;

    uint32_t attachCount = 0;
    bool deleteStatus = false;
};

/// An active vertex attribute, as getActiveAttrib reports it.
struct VkAttribInfo {
    std::string name;
    GLenum type = GL_FLOAT_VEC4;
    GLint size = 1;
    GLint location = -1;
};

/// One vertex input location a linked program reads (a matrix attribute
/// takes one per column), with the shader's component type.
struct VkVertexInput {
    uint32_t location = 0;
    enum class Kind : uint8_t { Float, Int, Uint } kind = Kind::Float;
};

/// An active uniform, as getActiveUniform / getActiveUniforms report it: a
/// default-block value, a sampler, or a member of a uniform block.
struct VkUniformInfo {
    std::string name;       // GL name; an array's ends in "[0]"
    GLenum type = GL_FLOAT;
    GLint size = 1;         // array length, 1 when not an array
    GLint blockIndex = -1;  // GL uniform block, -1 for the default block
    // Layout in its block (std140). Default-block values are laid out the
    // same way in the program's own uniform buffer, though GL reports -1 for
    // their offset and strides.
    uint32_t offset = 0;
    uint32_t arrayStride = 0;
    uint32_t matrixStride = 0;
    bool rowMajor = false;
    GLint location = -1;    // first GL location (default block and samplers)
    int32_t sampler = -1;   // index into ProgramInterface::samplers
};

/// A sampler uniform's descriptor binding (an array of samplers is one
/// binding of `count` descriptors) and the texture unit each element reads.
struct VkSamplerBinding {
    uint32_t binding = 0;
    uint32_t count = 1;
    GLenum type = GL_SAMPLER_2D;
    uint32_t firstUnit = 0;  // index of element 0 in VkProgramResource::samplerUnits
    VkShaderStageFlags stages = 0;  // the stages that sample it
};

/// An active uniform block. An array of blocks is one block per element in
/// GL, each its own descriptor of one binding in Vulkan.
struct VkUniformBlockInfo {
    std::string name;
    uint32_t dataSize = 0;
    std::vector<GLuint> activeUniformIndices;
    bool referencedByVertex = false;
    bool referencedByFragment = false;
    uint32_t descriptorBinding = 0;
    uint32_t arrayElement = 0;
};

/// A GL uniform location: which uniform, and which element of it.
struct VkUniformLocation {
    uint32_t uniform = 0;
    uint32_t element = 0;
};

/// What linking learns about a program from glslang's reflection.
struct ProgramInterface {
    std::vector<VkAttribInfo> attribs;
    std::vector<VkVertexInput> vertexInputs;
    std::unordered_map<std::string, GLint> fragDataLocations;
    std::vector<VkUniformInfo> uniforms;
    std::vector<VkUniformLocation> locations;  // indexed by GL location
    std::vector<VkUniformBlockInfo> uniformBlocks;
    std::vector<VkSamplerBinding> samplers;
    uint32_t samplerUnitCount = 0;
    uint32_t vertexSamplerUnits = 0;    // of samplerUnitCount, those each stage uses:
    uint32_t fragmentSamplerUnits = 0;  // a sampler both use counts in both
    int32_t defaultBlockBinding = -1;  // -1: no default-block values
    uint32_t defaultBlockSize = 0;
};

/// A program object. A successful link replaces the executable — modules,
/// layouts, interface — and resets uniform values and block bindings.
struct VkProgramResource {
    GLuint vertShaderId = 0;
    GLuint fragShaderId = 0;
    bool linkStatus = false;
    bool deleteStatus = false;
    std::string infoLog;
    std::unordered_map<std::string, GLuint> boundAttribLocations;  // bindAttribLocation

    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    // One set: the program's samplers, uniform blocks and default-block
    // buffer at the bindings glslang mapped them to.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    ProgramInterface iface;
    std::vector<uint8_t> uniformBytes;     // the default block, std140
    std::vector<uint32_t> samplerUnits;    // texture unit per sampler element
    std::vector<GLuint> blockBindings;     // GL binding point per uniform block

    // What the last draw bound, reused by the next draw in the same stream
    // segment when nothing changed (webgl_vk_context_draw.cpp). Descriptor
    // sets and the uniform slice come from the segment, so they never
    // outlive it.
    uint64_t drawSegmentSerial = 0;
    std::vector<uint8_t> drawUniformBytes;
    VkDescriptorBufferInfo drawUniforms{};
    std::vector<uint64_t> drawBindingKey;
    VkDescriptorSet drawSet = VK_NULL_HANDLE;

    bool isValid() const { return linkStatus; }
};

/// What a framebuffer attachment point holds: a texture image (one level of
/// one layer or cube face) or a renderbuffer.
struct VkFboAttachment {
    enum class Kind : uint8_t { None, Texture, Renderbuffer };
    Kind kind = Kind::None;
    GLuint id = 0;
    uint32_t level = 0;
    uint32_t layer = 0;
    bool operator==(const VkFboAttachment&) const = default;
};

/// Framebuffer object. drawBuffers[i] is COLOR_ATTACHMENTi or NONE: what
/// fragment output i writes, as GL ES 3 defines it.
struct VkFramebufferResource {
    std::array<VkFboAttachment, 8> color{};
    VkFboAttachment depth;
    VkFboAttachment stencil;
    std::array<GLenum, 8> drawBuffers{0x8CE0 /* GL_COLOR_ATTACHMENT0 */};
    GLenum readBuffer = 0x8CE0;
};

/// Renderbuffer object: its own image (multisampled when asked), never
/// sampled, kept in attachment layout between uses.
struct VkRenderbufferResource {
    VkTextureResource storage;
    GLenum internalformat = 0;
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

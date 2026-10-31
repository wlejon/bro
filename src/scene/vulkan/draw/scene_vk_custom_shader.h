#pragma once

// Reflection of a brokernel hook module's set-4 interface (custom.bk):
//   - binding 0: uniform<Params>, its fields matched by name to setShaderUniform
//   - bindings 1..kMaxSamplers: texture2d / texture2d_array / texture3d
//   - bindings 10..13: custom.bk's immutable samplers (linear, repeat, repeat_s, nearest)
//   - bindings 14..23: storage buffers (read_buffer<T> / buffer<T>), matched by name to setShaderBuffer

#include "scene/nodes/mesh_node.h"
#include "scene/scene_renderer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace brokernel {
struct CompileResult;
}

namespace bro::scene::vk {

class CustomShaderInterface {
public:
    static constexpr uint32_t kMaxSamplers = MeshNode::kMaxUserTextures;
    enum SamplerMode : uint32_t {
        kLinear = 0,    // linear, mips, clamped
        kRepeat = 1,    // linear, mips, repeating
        kRepeatS = 2,   // linear, mips, repeating across (s), clamped in t
        kNearest = 3,   // nearest, clamped (integer images)
        kSamplerModes = 4,
    };
    static constexpr uint32_t kKernelSamplersBinding = 10;
    static constexpr uint32_t kBuffersBinding = kKernelSamplersBinding + kSamplerModes;
    static constexpr uint32_t kMaxBuffers = 10;

    struct Uniform {
        std::string type;
        std::string name;
        uint32_t offset = 0;   // std140 offset
        bool integer = false;  // i32 / u32: values convert from float
    };

    struct Sampler {
        std::string name;
        bool array = false;   // texture2d_array
        bool volume = false;  // texture3d
    };

    /// A storage buffer of a brokernel hook (setShaderBuffer by name).
    struct KernelBuffer {
        std::string name;
        uint32_t binding = 0;
    };

    /// Reflect set 4 from the hook's compiled stage(s). False with `err` on a
    /// resource of set 4 outside the custom.bk layout.
    bool fromKernel(std::initializer_list<const brokernel::CompileResult*> stages, std::string& err);

    const std::vector<Uniform>& uniforms() const { return uniforms_; }
    const std::vector<Sampler>& samplers() const { return samplers_; }
    const std::vector<KernelBuffer>& kernelBuffers() const { return kernelBuffers_; }
    uint32_t uboSize() const { return uboSize_; }

private:
    std::vector<Uniform> uniforms_;
    std::vector<Sampler> samplers_;
    std::vector<KernelBuffer> kernelBuffers_;
    uint32_t uboSize_ = 16;
};

} // namespace bro::scene::vk

#include "scene/vulkan/draw/scene_vk_custom_shader.h"

#include <brokernel/brokernel.h>

#include <algorithm>

namespace bro::scene::vk {

namespace {
constexpr uint32_t kCustomSet = 4;
} // namespace

bool CustomShaderInterface::fromKernel(std::initializer_list<const brokernel::CompileResult*> stages,
                                       std::string& err) {
    uniforms_.clear();
    samplers_.clear();
    kernelBuffers_.clear();
    uboSize_ = 16;
    bool haveUniform = false;
    for (const brokernel::CompileResult* stage : stages) {
        for (const brokernel::ResourceInfo& r : stage->resources) {
            if (r.set != kCustomSet) continue;
            const brokernel::Type& t = r.type;
            const std::string where = r.module + "::" + r.name + " at (4, " + std::to_string(r.binding) + ")";
            if (r.binding == 0) {
                if (!t.is_uniform() || t.struct_id == UINT32_MAX) {
                    err = where + " is not a uniform<struct>";
                    return false;
                }
                if (haveUniform) continue;
                haveUniform = true;
                const brokernel::StructDef* sdef = stage->types.find_struct(t.struct_id);
                if (!sdef) {
                    err = where + ": no struct";
                    return false;
                }
                uint32_t end = 0;
                for (const brokernel::StructField& f : sdef->fields) {
                    const brokernel::Type& ft = f.type;
                    const bool numeric = (ft.is_scalar() || ft.is_vector()) &&
                                         (ft.scalar == brokernel::ScalarKind::F32 ||
                                          ft.scalar == brokernel::ScalarKind::I32 ||
                                          ft.scalar == brokernel::ScalarKind::U32);
                    if (!numeric) {
                        err = where + ": field " + f.name + " is not a 32-bit scalar or vector";
                        return false;
                    }
                    const uint32_t lanes = ft.is_vector() ? ft.dimension : 1;
                    Uniform u;
                    u.name = f.name;
                    u.type = lanes == 1 ? "scalar" : "vec" + std::to_string(lanes);
                    u.offset = stage->types.block_field_offset(*sdef, f.name, true);
                    u.integer = ft.scalar != brokernel::ScalarKind::F32;
                    end = std::max(end, u.offset + 4 * lanes);
                    uniforms_.push_back(std::move(u));
                }
                uboSize_ = std::max<uint32_t>(16, (end + 15) / 16 * 16);
            } else if (r.binding <= kMaxSamplers) {
                if (!t.is_sampler() || !t.image_separate() || t.image_cube() ||
                    (t.image_flags & brokernel::Type::kImageShadow) || t.dimension < 2) {
                    err = where + " is not a texture2d, texture2d_array or texture3d";
                    return false;
                }
                if (samplers_.size() < r.binding) samplers_.resize(r.binding);
                Sampler& s = samplers_[r.binding - 1];
                s.name = r.name;
                s.array = t.image_arrayed();
                s.volume = t.dimension == 3;
            } else if (r.binding >= kKernelSamplersBinding && r.binding < kBuffersBinding) {
                if (!t.is_sampler() || !t.sampler_only()) {
                    err = where + " is one of custom.bk's samplers";
                    return false;
                }
            } else if (r.binding >= kBuffersBinding && r.binding < kBuffersBinding + kMaxBuffers) {
                if (!t.is_buffer()) {
                    err = where + " is not a buffer or read_buffer";
                    return false;
                }
                bool known = false;
                for (const KernelBuffer& b : kernelBuffers_) known |= b.binding == r.binding;
                if (!known) kernelBuffers_.push_back({r.name, r.binding});
            } else {
                err = where + " is outside set 4 (custom.bk)";
                return false;
            }
        }
    }
    return true;
}

} // namespace bro::scene::vk

#include "scene/vulkan/lighting/scene_atmosphere.h"

#include "render/vulkan_queue.h"
#include "render/vulkan_util.h"
#include "scene/nodes/light_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/world/atmosphere.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

constexpr BuiltinSceneShader kShaders[] = {
    BuiltinSceneShader::AtmosphereTransmittanceComp, BuiltinSceneShader::AtmosphereMultiScatterComp,
    BuiltinSceneShader::AtmosphereSkyLightComp, BuiltinSceneShader::AtmosphereSkyViewComp,
    BuiltinSceneShader::AtmosphereAerialComp};
constexpr const char* kNames[] = {"atmosphere transmittance", "atmosphere multiscatter", "atmosphere skylight",
                                  "atmosphere skyview", "atmosphere aerial"};

// The sky view is remade when the eye's height moves by more than this
// share of its altitude (at least a metre), or the sun's cosine by more
// than kSkyViewMuS: well under what a table texel spans there.
constexpr float kSkyViewHeightShare = 0.01f;
constexpr float kSkyViewMuS = 1e-4f;
// The eye's heights (m) the air's lookups are all the froxels' under and
// none of them over (between, a smoothstep).
constexpr double kFroxelsFull = 15000.0;
constexpr double kFroxelsNone = 40000.0;

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(man | 1024u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

uint64_t hashFloats(std::initializer_list<float> values) {
    uint64_t h = 1469598103934665603ull;
    for (float f : values) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        for (int i = 0; i < 4; ++i) {
            h ^= (u >> (8 * i)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

void normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-8f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    } else {
        v[0] = 0.0f;
        v[1] = 1.0f;
        v[2] = 0.0f;
    }
}

}  // namespace

bool SceneAtmosphere::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    SceneVkDescriptorLayoutBuilder b;
    b.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = b.build(dev);
    if (!setLayout_) return false;
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &layout_) != VK_SUCCESS) return false;
    for (int j = 0; j < kJobs; ++j) {
        pipelines_[j] = createComputePipeline(dev, kShaders[j], layout_, kNames[j]);
        if (!pipelines_[j]) return false;
    }

    using namespace atmosphere;
    SceneVkAllocator& a = gpu.allocator;
    if (!a.createStorageImage(kTransmittanceW, kTransmittanceH, 1, kFormat, transmittance_, true) ||
        !a.createStorageImage(kMultiScatterSun, kMultiScatterAlt, 1, kFormat, multiScatter_, true) ||
        !a.createStorageImage(kSkyLightSun, kSkyLightAlt, kSkyLightLayers, kFormat, skyLight_, true))
        return false;
    slots_.resize(render::VulkanFrames::kFramesInFlight);
    for (Slot& s : slots_) {
        if (!a.createStorageImage(kSkyViewW, kSkyViewH, 1, kFormat, s.skyView, true) ||
            !a.createStorageImage(kFroxelXY, kFroxelXY, kFroxelSlices, kFormat, s.inscatter, true) ||
            !a.createStorageImage(kFroxelXY, kFroxelXY, kFroxelSlices, kFormat, s.transmittance, true))
            return false;
    }
    kind_ = gpu.world.addKind(this, {"atmosphere", WorldSchedule::Compute, 0.05f});
    return kind_ >= 0;
}

void SceneAtmosphere::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    SceneVkAllocator& a = gpu.allocator;
    for (SceneVkImage* img : {&transmittance_, &multiScatter_, &skyLight_}) a.destroyImage(*img);
    for (Slot& s : slots_)
        for (SceneVkImage* img : {&s.skyView, &s.inscatter, &s.transmittance}) a.destroyImage(*img);
    slots_.clear();
    for (VkPipeline& p : pipelines_) {
        if (p) gpu.device.defer([dev, pipeline = p] { vkDestroyPipeline(dev, pipeline, nullptr); });
        p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    if (sampler_) gpu.device.defer([dev, s = sampler_] { vkDestroySampler(dev, s, nullptr); });
    sampler_ = VK_NULL_HANDLE;
}

void SceneAtmosphere::originFor(const bromath::Vec3& eye, float out[4]) const {
    const double Rb = block_.shell[0];
    out[3] = 0.0f;
    if (block_.shell[3] > 0.5f) {
        // Spherical: the eye about the planet's centre, kept a metre above its surface.
        double d[3] = {static_cast<double>(eye.x) - center_[0], static_cast<double>(eye.y) - center_[1],
                       static_cast<double>(eye.z) - center_[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const double r = std::max(len, Rb + 1.0);
        for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(len > 1e-6 ? d[k] * r / len : (k == 1 ? r : 0.0));
        return;
    }
    // Flat: the planet straight below the eye.
    out[0] = 0.0f;
    out[1] = static_cast<float>(std::max(static_cast<double>(eye.y) - seaLevel_, 1.0) + Rb);
    out[2] = 0.0f;
}

void SceneAtmosphere::beginFrame(SceneFrame& frame) {
    const SceneRenderer& r = frame.renderer;
    const AtmosphereParams& a = r.atmosphere();
    on_ = a.enabled && !slots_.empty();
    tablesOnCompute_ = false;
    block_ = AtmosphereGpu{};
    block_.ground[3] = -1.0f;
    if (!on_) return;
    rendered_ = true;

    // The sun: the brightest directional light, unless the params name it.
    const std::vector<LightNode*>& lights = r.frameLights();
    int sunLight = -1;
    float best = -1.0f;
    for (int i = 0; i < static_cast<int>(lights.size()) && i < kSceneMaxLights; ++i) {
        const LightNode* l = lights[static_cast<size_t>(i)];
        if (!l || l->kind() != LightNode::Kind::Directional) continue;
        const bromath::Vec3& c = l->color();
        const float power = (c.x + c.y + c.z) * l->intensity();
        if (power > best) {
            best = power;
            sunLight = i;
        }
    }
    float sunDir[3] = {a.sunDir[0], a.sunDir[1], a.sunDir[2]};
    float illum[3] = {a.sunColor[0], a.sunColor[1], a.sunColor[2]};
    if (sunLight >= 0) {
        const LightNode& l = *lights[static_cast<size_t>(sunLight)];
        if (!a.sunDirExplicit) {
            const bromath::Vec3 d = l.direction();
            sunDir[0] = -d.x;
            sunDir[1] = -d.y;
            sunDir[2] = -d.z;
        }
        if (!a.sunColorExplicit) {
            illum[0] = l.color().x * l.intensity();
            illum[1] = l.color().y * l.intensity();
            illum[2] = l.color().z * l.intensity();
        }
    }
    normalize3(sunDir);

    AtmosphereGpu& g = block_;
    std::copy(sunDir, sunDir + 3, g.sunDir);
    g.sunDir[3] = 1.0f;
    std::copy(illum, illum + 3, g.sunIlluminance);
    g.sunIlluminance[3] = a.sunAngularRadius;
    std::copy(a.rayleigh, a.rayleigh + 3, g.rayleigh);
    g.rayleigh[3] = a.scaleHeightR;
    g.mie[0] = a.mieScattering;
    g.mie[1] = a.mieAbsorption;
    g.mie[2] = a.mieG;
    g.mie[3] = a.scaleHeightM;
    std::copy(a.ozone, a.ozone + 3, g.ozone);
    g.ozone[3] = a.ozoneCenter;
    g.shell[0] = a.planetRadius;
    g.shell[1] = a.planetRadius + a.thickness;
    g.shell[2] = std::max(a.ozoneWidth, 1.0f);
    g.shell[3] = 0.0f;
    std::copy(a.groundAlbedo, a.groundAlbedo + 3, g.ground);
    g.ground[3] = static_cast<float>(sunLight);
    seaLevel_ = a.seaLevel;
    eye_ = frame.world.eye;   // its centre and sea level are the body's
    // On a round body the air wraps the body: its radius, about its centre
    // where the body's frame (the place's axes, its origin added) has it.
    const Place& place = frame.graph.place();
    if (!place.body().flat()) {
        const double zero[3] = {0.0, 0.0, 0.0};
        double c[3];
        place.toPlace(zero, c);
        for (int k = 0; k < 3; ++k) center_[k] = c[k] + frame.view.origin[k];
        g.shell[0] = static_cast<float>(place.body().radius);
        g.shell[1] = g.shell[0] + a.thickness;
        g.shell[3] = 1.0f;
    }
    originFor(eye_, g.origin);

    // The tables' inputs: a change remakes them (the world-service jobs).
    const uint64_t hash = hashFloats({g.rayleigh[0], g.rayleigh[1], g.rayleigh[2], g.rayleigh[3], g.mie[0],
                                      g.mie[1], g.mie[2], g.mie[3], g.ozone[0], g.ozone[1], g.ozone[2], g.ozone[3],
                                      g.shell[0], g.shell[1], g.shell[2], g.ground[0], g.ground[1], g.ground[2],
                                      g.sunIlluminance[3]});
    if (hash != wantHash_) {
        wantHash_ = hash;
        ++version_;
    }

    // This slot's sky view: remade when the eye's height or the sun moved.
    slot_ = frame.gpu.device.frames().frameIndex() % static_cast<uint32_t>(slots_.size());
    Slot& slot = slots_[slot_];
    const double Rb = g.shell[0];
    const double rr = std::sqrt(static_cast<double>(g.origin[0]) * g.origin[0] +
                                static_cast<double>(g.origin[1]) * g.origin[1] +
                                static_cast<double>(g.origin[2]) * g.origin[2]);
    const float r0 = static_cast<float>(rr);
    const float muS = static_cast<float>((g.origin[0] * sunDir[0] + g.origin[1] * sunDir[1] +
                                          g.origin[2] * sunDir[2]) / rr);
    const float height = static_cast<float>(rr - Rb);
    remakeSkyView_ = !slot.valid || slot.version != version_ ||
                     std::fabs(r0 - slot.r) > std::max(1.0f, kSkyViewHeightShare * height) ||
                     std::fabs(muS - slot.muS) > kSkyViewMuS;
    if (remakeSkyView_) {
        slot.valid = true;
        slot.version = version_;
        slot.r = r0;
        slot.muS = muS;
    }
    g.skyView[0] = slot.r;
    g.skyView[1] = slot.muS;
    g.skyView[2] = 1.0f;   // dither the lookups
    // The froxels' share of the air's lookups (scene_lighting.glsl's
    // sceneAtmosphereAir): all of it low in the air; none from high in it
    // or above, where a slice (squared in depth out to the air's horizon)
    // is deeper than the air under the eye and its dither would be noise.
    {
        const double t = std::clamp((static_cast<double>(height) - kFroxelsFull) / (kFroxelsNone - kFroxelsFull), 0.0, 1.0);
        g.skyView[3] = static_cast<float>(1.0 - t * t * (3.0 - 2.0 * t));
    }

    // The froxels over the main camera's frustum, out to its far plane or
    // the air's horizon, whichever is nearer.
    const SceneView& v = frame.view;
    const double Rt = g.shell[1];
    const double horizon = std::sqrt(std::max(rr * rr - Rb * Rb, 0.0)) + std::sqrt(std::max(Rt * Rt - Rb * Rb, 0.0));
    const double far = std::isfinite(v.farZ) && v.farZ > 0.0f ? v.farZ : horizon;
    g.froxel[3] = static_cast<float>(std::clamp(far, 100.0, std::max(horizon, 100.0)));
    if (v.perspective) {
        std::memcpy(g.froxelViewProj, v.relViewProj.data, sizeof(g.froxelViewProj));
        const bromath::Mat4 inv = bromath::minverse(v.relViewProj);
        std::memcpy(froxelInv_, inv.data, sizeof(froxelInv_));
    } else {
        // An orthographic view has no eye to march from: every lookup takes
        // the tables' path outside the volume (sceneAtmosphereAir).
        std::memset(g.froxelViewProj, 0, sizeof(g.froxelViewProj));
        std::memset(froxelInv_, 0, sizeof(froxelInv_));
    }
}

void SceneAtmosphere::fill(AtmosphereGpu& out, const bromath::Vec3& eye) const {
    out = block_;
    if (!on_) return;
    originFor(eye, out.origin);
    out.froxel[0] = eye.x - eye_.x;
    out.froxel[1] = eye.y - eye_.y;
    out.froxel[2] = eye.z - eye_.z;
}

void SceneAtmosphere::writeBindings(SceneVkDescriptorWriter& writer) const {
    const Slot& s = slots_[slot_];
    auto image = [&](uint32_t binding, VkImageView view) {
        writer.writeImage(binding, view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    };
    image(9, transmittance_.view);
    image(10, s.skyView.view);
    image(11, s.inscatter.view);
    image(12, s.transmittance.view);
    image(13, skyLight_.view);
}

// Before: what earlier work read of the images to be written has finished.
// After: what was written is visible to what reads it next. On the compute
// queue only compute stages exist; the frame's join (a timeline wait) is
// the graphics' dependency there.
void SceneAtmosphere::barrier(VkCommandBuffer cmd, bool apart, bool before) {
    constexpr VkPipelineStageFlags kGraphics =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags other = apart ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : kGraphics;
    if (before) {
        render::cmdMemoryBarrier(cmd, other, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    } else {
        render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, other,
                                 VK_ACCESS_SHADER_READ_BIT);
    }
}

void SceneAtmosphere::dispatch(SceneFrame& frame, VkCommandBuffer cmd, Job job, const Slot& slot, float r,
                               float muS) {
    Block block{};
    block.atm = block_;
    std::memcpy(block.froxelInvViewProj, froxelInv_, sizeof(froxelInv_));
    block.job[0] = r;
    block.job[1] = muS;
    SceneVkDevice& device = frame.gpu.device;
    const VkDescriptorBufferInfo ubo = device.frameUniform(&block, sizeof(block));
    VkDescriptorSet set = device.frameSet(setLayout_);
    SceneVkDescriptorWriter w;
    w.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    w.writeImage(1, transmittance_.view, sampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(2, multiScatter_.view, sampler_, VK_IMAGE_LAYOUT_GENERAL);
    auto storage = [&](uint32_t binding, const SceneVkImage& img) {
        w.writeImage(binding, img.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    };
    using namespace atmosphere;
    uint32_t gx = 1, gy = 1;
    switch (job) {
        case kTransmittance:
            storage(3, transmittance_);
            storage(4, transmittance_);
            gx = kTransmittanceW / 8;
            gy = kTransmittanceH / 8;
            break;
        case kMultiScatter:
            storage(3, multiScatter_);
            storage(4, multiScatter_);
            gx = kMultiScatterSun;
            gy = kMultiScatterAlt;
            break;
        case kSkyLight:
            storage(3, skyLight_);
            storage(4, skyLight_);
            gx = kSkyLightSun;
            gy = kSkyLightAlt;
            break;
        case kSkyView:
            storage(3, slot.skyView);
            storage(4, slot.skyView);
            gx = kSkyViewW / 8;
            gy = kSkyViewH / 8;
            break;
        case kAerial:
            storage(3, slot.inscatter);
            storage(4, slot.transmittance);
            // atmosphere_aerial.comp: a workgroup a pair of columns, an
            // invocation a slice.
            static_assert(kFroxelSlices == 32 && kFroxelXY % 2 == 0);
            gx = kFroxelXY;
            gy = kFroxelXY / 2;
            break;
        case kJobs:
            return;
    }
    w.updateSet(device.device(), set);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[job]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, gx, gy, 1);
}

void SceneAtmosphere::offer(SceneFrame&, WorldService& service) {
    if (!on_ || (made_ && madeHash_ == wantHash_)) return;
    // The three in order, each the next one's input; all required.
    for (uint32_t j = kTransmittance; j <= kSkyLight; ++j) {
        WorldJob job;
        job.key = j;
        job.kind = kind_;
        job.error = static_cast<float>(3 - j);
        job.size = 1.0;
        job.required = true;
        service.offer(job);
    }
}

double SceneAtmosphere::record(SceneFrame& frame, int, std::span<const WorldJob> granted, WorldStream& stream) {
    if (granted.empty()) return 0;
    VkCommandBuffer cmd = stream.cmd();
    const bool apart = stream.apart();
    // On its own queue the rewrite follows what the graphics read of the
    // tables in earlier frames.
    if (apart) frame.gpu.device.computeReadsGraphics();
    barrier(cmd, apart, true);
    uint32_t n = 0;
    bool all[3] = {false, false, false};
    for (const WorldJob& job : granted) {
        if (job.key > kSkyLight) continue;
        if (n > 0) barrier(cmd, true, false);
        dispatch(frame, cmd, static_cast<Job>(job.key), slots_[slot_], 0.0f, 0.0f);
        all[job.key] = true;
        ++n;
    }
    barrier(cmd, apart, false);
    if (apart && n > 0) tablesOnCompute_ = true;
    if (all[0] && all[1] && all[2]) {
        madeHash_ = wantHash_;
        made_ = true;
    }
    return n;
}

void SceneAtmosphere::recordView(SceneFrame& frame) {
    if (!on_) return;
    const Slot& slot = slots_[slot_];
    // The sky view and the froxels in the graphics stream (see the header),
    // fenced from the reads of earlier frames and for this frame's. Both run
    // ahead of the frame's join, where the graphics would otherwise idle
    // waiting for the plants' plans; on a frame whose tables the world
    // service remade on the compute queue this pass is the join.
    VkCommandBuffer cmd = frame.cmd;
    barrier(cmd, false, true);
    if (remakeSkyView_) dispatch(frame, cmd, kSkyView, slot, slot.r, slot.muS);
    dispatch(frame, cmd, kAerial, slot, 0.0f, 0.0f);
    barrier(cmd, false, false);
}

bool SceneAtmosphere::read(SceneGpu& gpu, AtmosphereTables& out) {
    if (!rendered_ || slots_.empty()) return false;
    SceneVkDevice& device = gpu.device;
    device.waitIdle();
    const Slot& slot = slots_[slot_];
    struct Copy {
        const SceneVkImage* image;
        std::vector<float>* dst;
        SceneVkBuffer buffer;
    };
    Copy copies[] = {{&transmittance_, &out.transmittance, {}}, {&multiScatter_, &out.multiScatter, {}},
                     {&slot.skyView, &out.skyView, {}},           {&skyLight_, &out.skyLight, {}},
                     {&slot.inscatter, &out.inscatter, {}},       {&slot.transmittance, &out.airTransmittance, {}}};
    VkCommandBuffer cmd = device.frames().beginCommands();
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bool ok = true;
    for (Copy& c : copies) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(c.image->width) * c.image->height * c.image->depth * 8;
        ok = ok && gpu.allocator.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                              c.buffer) &&
             c.buffer.mappedData;
        if (!ok) break;
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {c.image->width, c.image->height, c.image->depth};
        vkCmdCopyImageToBuffer(cmd, c.image->image, VK_IMAGE_LAYOUT_GENERAL, c.buffer.buffer, 1, &region);
    }
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    const uint64_t ticket = device.frames().submit(cmd);
    ok = ok && ticket != 0 && device.context().queue().wait(ticket);
    for (Copy& c : copies) {
        if (ok) {
            const size_t n = static_cast<size_t>(c.image->width) * c.image->height * c.image->depth * 4;
            const auto* src = static_cast<const uint16_t*>(c.buffer.mappedData);
            c.dst->resize(n);
            for (size_t i = 0; i < n; ++i) (*c.dst)[i] = halfToFloat(src[i]);
        }
        gpu.allocator.destroyBuffer(c.buffer);
    }
    out.block = block_;
    std::memcpy(out.froxelInvViewProj, froxelInv_, sizeof(froxelInv_));
    return ok;
}

}  // namespace bro::scene::vk

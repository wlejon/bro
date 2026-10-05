#include "scene/vulkan/pass_billboard.h"

#include "scene/html_node.h"
#include "scene/light_node.h"
#include "scene/scene_graph.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"

#include <bromath/color.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace bro::scene::vk {

bool PassBillboard::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    const VkDescriptorSetLayout sets[2] = {gpu.defaults.cameraLayout, gpu.defaults.materialLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = sets;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;
    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BillboardVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BillboardFrag);
    return vs_ && fs_;
}

void PassBillboard::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
}

void PassBillboard::declare(const SceneFrame&, PassIO& io) const {
    io.hdr();
}

VkDescriptorSet PassBillboard::textureSet(SceneFrame& frame, const NodeTexture& tex, uint32_t nodeId,
                                          TextureSlot slot) {
    const SceneVkImage* img = frame.gpu.resources.texture(nodeId, slot, tex);
    if (!img) return VK_NULL_HANDLE;
    const SceneDefaults& d = frame.gpu.defaults;
    VkDescriptorSet set = device_->frameSet(d.materialLayout);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, img->view, img->sampler);
    writer.writeImage(1, d.flatNormal.view, d.sampler);
    writer.writeImage(2, d.white.view, d.sampler);
    writer.writeImage(3, d.black.view, d.sampler);
    writer.updateSet(device_->device(), set);
    return set;
}

bool PassBillboard::prepareNode(SceneFrame& frame, SceneNode& node, Push& push, VkDescriptorSet& material) {
    push.uvMax[0] = push.uvMax[1] = 1.0f;
    push.color[0] = push.color[1] = push.color[2] = push.color[3] = 1.0f;
    float halfW = 0.5f, halfH = 0.5f;
    const bromath::Vec3& scale = node.scale();

    if (node.type() == SceneNode::Type::Shape) {
        auto& s = static_cast<ShapeNode&>(node);
        switch (s.shape()) {
        case ShapeNode::Shape::Circle:
            push.shapeMode = 1;
            halfW = s.radius() * scale.x;
            halfH = s.radius() * scale.y;
            break;
        case ShapeNode::Shape::Ellipse:
            push.shapeMode = 1;
            halfW = s.radiusX() * scale.x;
            halfH = s.radiusY() * scale.y;
            break;
        default:   // rect, round rect and the rest draw as their box
            push.shapeMode = 0;
            halfW = 0.5f * s.width() * scale.x;
            halfH = 0.5f * s.height() * scale.y;
            break;
        }
        const auto& fill = s.fillColor();
        push.color[0] = bromath::clinearToSrgb(fill.r);
        push.color[1] = bromath::clinearToSrgb(fill.g);
        push.color[2] = bromath::clinearToSrgb(fill.b);
        push.color[3] = s.hasFill() ? fill.a : 0.0f;
        const auto& stroke = s.strokeColor();
        push.stroke[0] = bromath::clinearToSrgb(stroke.r);
        push.stroke[1] = bromath::clinearToSrgb(stroke.g);
        push.stroke[2] = bromath::clinearToSrgb(stroke.b);
        push.stroke[3] = stroke.a;
        const float uvRef = std::max(halfW, halfH) * 2.0f;
        push.strokeWidth = (s.hasStroke() && uvRef > 0.0f) ? s.strokeWidth() / uvRef : 0.0f;
    } else if (node.type() == SceneNode::Type::Sprite) {
        auto& s = static_cast<SpriteNode&>(node);
        float worldW = s.width();
        float worldH = s.height();
        if (worldW <= 0.0f || worldH <= 0.0f) {
            float sx, sy, sw, sh;
            if (s.currentSheetRect(sx, sy, sw, sh)) {
                if (worldW <= 0.0f) worldW = sw;
                if (worldH <= 0.0f) worldH = sh;
            } else if (s.imageWidth() > 0 && s.imageHeight() > 0) {
                if (worldW <= 0.0f) worldW = static_cast<float>(s.imageWidth());
                if (worldH <= 0.0f) worldH = static_cast<float>(s.imageHeight());
            }
        }
        push.shapeMode = 4;
        halfW = 0.5f * worldW * scale.x;
        halfH = 0.5f * worldH * scale.y;
        push.color[3] = s.opacity();
        s.ensureImageLoaded();
        s.currentUvRect(push.uvMin[0], push.uvMin[1], push.uvMax[0], push.uvMax[1]);
        material = s.hasImage() ? textureSet(frame, s.image(), s.id(), TextureSlot::Sprite) : VK_NULL_HANDLE;
        if (!s.hasImage()) push.color[3] = 0.0f;
    } else if (node.type() == SceneNode::Type::Html) {
        auto& h = static_cast<HtmlNode&>(node);
        const float ppu = h.pxPerUnit() > 0.0f ? h.pxPerUnit() : 100.0f;
        push.shapeMode = 2;
        halfW = 0.5f * (h.layoutWidth() / ppu) * scale.x;
        halfH = 0.5f * (h.layoutHeight() / ppu) * scale.y;
        material = textureSet(frame, h.texture(), h.id(), TextureSlot::Html);
        push.color[3] = h.texture().empty() ? 0.0f : 1.0f;
    } else {
        return false;
    }
    if (push.color[3] <= 0.0f && push.shapeMode != 2) return false;

    const bromath::Vec3 anchor = node.worldAnchor();
    if (const bromath::Frustum* frustum = frame.renderer.cullingFrustum()) {
        const float r = std::sqrt(halfW * halfW + halfH * halfH);
        if (!bromath::fintersects(*frustum, bromath::Sphere{anchor, r})) {
            frame.stats.billboardsCulled++;
            return false;
        }
    }
    frame.stats.billboardsDrawn++;

    bromath::Vec3 right = frame.view.right();
    bromath::Vec3 up = frame.view.up();
    if (node.billboardMode() == SceneNode::BillboardMode::YLock && std::abs(frame.view.forward().y) < 0.99f) {
        up = {0.0f, 1.0f, 0.0f};
        const bromath::Vec3 flat{right.x, 0.0f, right.z};
        const float len = bromath::vlen(flat);
        if (len > 1e-5f) right = flat * (1.0f / len);
    }
    // Camera-relative, like every scene position the GPU sees.
    const bromath::Vec3 rel = frame.view.relative(anchor);
    push.anchor[0] = rel.x;
    push.anchor[1] = rel.y;
    push.anchor[2] = rel.z;
    push.right[0] = right.x;
    push.right[1] = right.y;
    push.right[2] = right.z;
    push.up[0] = up.x;
    push.up[1] = up.y;
    push.up[2] = up.z;
    push.halfSize[0] = halfW;
    push.halfSize[1] = halfH;
    return true;
}

void PassBillboard::prepareLightIcon(const SceneFrame& frame, const LightNode& light, Push& push) {
    const auto& m = light.worldMatrix();
    const bromath::Vec3 right = frame.view.right();
    const bromath::Vec3 up = frame.view.up();
    const bromath::Vec3 rel = frame.view.relative(bromath::Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)});
    push.anchor[0] = rel.x;
    push.anchor[1] = rel.y;
    push.anchor[2] = rel.z;
    push.right[0] = right.x;
    push.right[1] = right.y;
    push.right[2] = right.z;
    push.up[0] = up.x;
    push.up[1] = up.y;
    push.up[2] = up.z;

    // Dark lights get a lift so the icon stays visible.
    const auto& c = light.color();
    const float lum = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    const float lift = lum < 0.2f ? 0.2f : 0.0f;
    push.color[0] = c.x + lift;
    push.color[1] = c.y + lift;
    push.color[2] = c.z + lift;
    push.color[3] = 1.0f;

    float half = 0.22f;
    float ring = 0.12f;
    if (light.kind() == LightNode::Kind::Directional) {
        half = 0.30f;
        ring = 0.18f;
        push.stroke[0] = push.stroke[1] = push.stroke[2] = 1.0f;
    } else if (light.kind() == LightNode::Kind::Point) {
        for (int i = 0; i < 3; ++i) push.stroke[i] = push.color[i] * 0.5f;
    } else {
        ring = 0.28f;
        for (int i = 0; i < 3; ++i) push.stroke[i] = std::min(push.color[i] * 0.8f, 1.0f);
    }
    push.stroke[3] = 1.0f;
    push.halfSize[0] = push.halfSize[1] = half;
    push.shapeMode = 3;
    push.strokeWidth = ring;
    push.uvMax[0] = push.uvMax[1] = 1.0f;
}

void PassBillboard::record(SceneFrame& frame) {
    std::vector<SceneNode*> anchored;
    for (SceneNode* n : frame.lists.nodes) {
        if (n->hasWorldAnchor()) anchored.push_back(n);
    }
    const bool icons = frame.renderer.showLightIcons();
    if (anchored.empty() && !icons) return;

    VkPipeline pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, fs_)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .setColorBlendAttachment(0, blend)
         .enableDepthTest(false);
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;

    VkCommandBuffer cmd = frame.cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &frame.cameraSet, 0, nullptr);
    auto draw = [&](const Push& push, VkDescriptorSet material) {
        VkDescriptorSet set = material ? material : frame.gpu.defaults.defaultMaterialSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        frame.drewContent = true;
    };

    for (SceneNode* node : anchored) {
        Push push{};
        VkDescriptorSet material = VK_NULL_HANDLE;
        if (prepareNode(frame, *node, push, material)) draw(push, material);
    }
    if (!icons) return;
    for (SceneNode* node : frame.lists.nodes) {
        if (node->type() != SceneNode::Type::Light) continue;
        Push push{};
        prepareLightIcon(frame, static_cast<const LightNode&>(*node), push);
        draw(push, VK_NULL_HANDLE);
    }
}

}  // namespace bro::scene::vk

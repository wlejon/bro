#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/html_node.h"
#include "scene/sprite_node.h"
#include "scene/shape_node.h"
#include "scene/particles3d_node.h"
#include "scene/decal_node.h"
#include "scene/light_node.h"
#include "util/log.h"
#include <bromath/color.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

void SceneVkBridge::renderDecalsPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                     CullStats& stats, bool& hasDrawnMeshes) {
    std::vector<DecalNode*> decals;
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::Decal) continue;
        auto* d = static_cast<DecalNode*>(node.get());
        if (renderer.cameraCulled(d)) {
            stats.decalsCulled++;
        } else {
            stats.decalsDrawn++;
            decals.push_back(d);
        }
    }

    if (decals.empty()) return;

    std::stable_sort(decals.begin(), decals.end(), [](DecalNode* a, DecalNode* b) {
        return a->renderPriority() < b->renderPriority();
    });

    passDecal_.begin(cmd, cameraSet_, lightingSet_);
    for (DecalNode* d : decals) {
        DecalPushConstants push{};
        std::memcpy(push.model, d->worldMatrix().data, sizeof(push.model));
        bromath::Mat4 invM = bromath::minverse(d->worldMatrix());
        std::memcpy(push.invModel, invM.data, sizeof(push.invModel));
        std::memcpy(push.modulate, d->modulate(), sizeof(push.modulate));

        const auto& w = d->worldMatrix();
        bromath::Vec3 up = bromath::vnorm(bromath::Vec3{w.at(0, 1), w.at(1, 1), w.at(2, 1)});
        push.decalUp[0] = up.x;
        push.decalUp[1] = up.y;
        push.decalUp[2] = up.z;
        push.decalUp[3] = d->emissionStrength();

        push.fades[0] = d->upperFade();
        push.fades[1] = d->lowerFade();
        push.fades[2] = d->normalFade();
        push.fades[3] = 0.0f;

        VkImageView albView = passDecal_.dummyWhiteView();
        VkSampler albSampler = passDecal_.defaultSampler();
        push.flags[0] = 0;
        const auto& pAlb = d->pendingAlbedo();
        if (d->hasAlbedoTexture() && pAlb.w > 0 && pAlb.h > 0 && !pAlb.data.empty()) {
            uploadTexture(&d->pendingAlbedo(), pAlb.w, pAlb.h, pAlb.data.data());
            auto it = textureCache_.find(&d->pendingAlbedo());
            if (it != textureCache_.end() && it->second.image.isValid()) {
                albView = it->second.image.view;
                albSampler = it->second.image.sampler;
                push.flags[0] = 1;
            }
        }

        VkImageView emView = passDecal_.dummyBlackView();
        VkSampler emSampler = passDecal_.defaultSampler();
        push.flags[1] = 0;
        const auto& pEm = d->pendingEmission();
        if (d->hasEmissionTexture() && pEm.w > 0 && pEm.h > 0 && !pEm.data.empty()) {
            uploadTexture(&d->pendingEmission(), pEm.w, pEm.h, pEm.data.data());
            auto it = textureCache_.find(&d->pendingEmission());
            if (it != textureCache_.end() && it->second.image.isValid()) {
                emView = it->second.image.view;
                emSampler = it->second.image.sampler;
                push.flags[1] = 1;
            }
        }

        VkDescriptorSet matSet = passDecal_.createMaterialSet(
            depthCopyImage_.view, depthCopyImage_.sampler,
            albView, albSampler, emView, emSampler, frameDescPool_);

        passDecal_.draw(cmd, push, matSet);
        hasDrawnMeshes = true;
    }
}

void SceneVkBridge::renderParticlesPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                        CullStats& stats, bool& hasDrawnMeshes) {
    std::vector<Particles3DNode*> particles;
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::Particles3D) continue;
        auto* p = static_cast<Particles3DNode*>(node.get());
        if (p->liveCount() <= 0) continue;
        if (renderer.cameraCulled(p)) {
            stats.particlesCulled++;
        } else {
            stats.particlesDrawn++;
            particles.push_back(p);
        }
    }

    if (particles.empty()) return;

    passParticles_.begin(cmd, cameraSet_);
    static const bromath::Mat4 kIdentity = bromath::midentity();
    bromath::Vec3 camFwd{-graph.viewMatrix().at(2, 0),
                         -graph.viewMatrix().at(2, 1),
                         -graph.viewMatrix().at(2, 2)};
    bromath::Vec3 camRight{graph.viewMatrix().at(0, 0),
                          graph.viewMatrix().at(0, 1),
                          graph.viewMatrix().at(0, 2)};
    bromath::Vec3 camUp{graph.viewMatrix().at(1, 0),
                        graph.viewMatrix().at(1, 1),
                        graph.viewMatrix().at(1, 2)};

    for (Particles3DNode* p : particles) {
        const auto& instData = p->buildInstanceData(camFwd);
        uint32_t activeCount = static_cast<uint32_t>(p->activeParticleCount());
        if (activeCount == 0 || instData.empty()) continue;

        auto& dynBuf = getDynamicBuffers(p);
        size_t instByteSize = activeCount * 10 * sizeof(float);
        if (dynBuf.instanceCapacity < instByteSize) {
            allocator_.destroyBuffer(dynBuf.instanceBuffer);
            allocator_.createVertexBuffer(instByteSize, instData.data(), dynBuf.instanceBuffer);
            dynBuf.instanceCapacity = instByteSize;
        } else {
            allocator_.stageAndUploadBuffer(dynBuf.instanceBuffer.buffer, instData.data(), instByteSize);
        }

        ParticlePushConstants push{};
        const bromath::Mat4& model = (p->space() == Particles3DNode::SimSpace::Local)
                                     ? p->worldMatrix() : kIdentity;
        std::memcpy(push.model, model.data, sizeof(push.model));
        push.camRight[0] = camRight.x; push.camRight[1] = camRight.y; push.camRight[2] = camRight.z;
        push.camUp[0] = camUp.x; push.camUp[1] = camUp.y; push.camUp[2] = camUp.z;
        push.params[0] = static_cast<float>(p->sheetCols());
        push.params[1] = static_cast<float>(p->sheetRows());
        push.params[2] = 0.0f;
        push.params[3] = p->softness();

        VkImageView texView = VK_NULL_HANDLE;
        VkSampler texSampler = VK_NULL_HANDLE;
        if (p->ensureTextureLoaded() && p->hasTexture() && !p->texturePixels().empty()) {
            uploadTexture(p, p->textureWidth(), p->textureHeight(), p->texturePixels().data());
            auto it = textureCache_.find(p);
            if (it != textureCache_.end() && it->second.image.isValid()) {
                texView = it->second.image.view;
                texSampler = it->second.image.sampler;
                push.params[2] = 1.0f;
            }
        }

        VkDescriptorSet pMatSet = passParticles_.createParticleMaterialSet(
            texView, texSampler,
            depthCopyImage_.view, depthCopyImage_.sampler,
            frameDescPool_);

        passParticles_.draw(cmd, p->blend() == Particles3DNode::Blend::Additive,
                            dynBuf.instanceBuffer.buffer, activeCount,
                            push, pMatSet);
        hasDrawnMeshes = true;
    }
}

void SceneVkBridge::renderBillboardsPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                         CullStats& stats, bool& hasDrawnMeshes) {
    std::vector<SceneNode*> billboardNodes;
    auto walkBB = [&](auto& self, SceneNode* n) -> void {
        if (!n || !n->renderVisible()) return;
        if (n->hasWorldAnchor()) {
            billboardNodes.push_back(n);
        }
        for (auto* c : n->children()) self(self, c);
    };
    walkBB(walkBB, graph.root_.get());

    bool hasLightIcons = renderer.showLightIcons();
    if (billboardNodes.empty() && !hasLightIcons) return;

    passBillboard_.begin(cmd, cameraSet_);

    bromath::Vec3 camRight{graph.viewMatrix().at(0, 0),
                          graph.viewMatrix().at(0, 1),
                          graph.viewMatrix().at(0, 2)};
    bromath::Vec3 camUp{graph.viewMatrix().at(1, 0),
                        graph.viewMatrix().at(1, 1),
                        graph.viewMatrix().at(1, 2)};
    bromath::Vec3 camForward{-graph.viewMatrix().at(2, 0),
                             -graph.viewMatrix().at(2, 1),
                             -graph.viewMatrix().at(2, 2)};

    for (SceneNode* node : billboardNodes) {
        BillboardPushConstants push{};
        push.uvMin[0] = 0.0f; push.uvMin[1] = 0.0f;
        push.uvMax[0] = 1.0f; push.uvMax[1] = 1.0f;
        push.color[0] = push.color[1] = push.color[2] = push.color[3] = 1.0f;
        push.stroke[0] = push.stroke[1] = push.stroke[2] = push.stroke[3] = 0.0f;
        push.strokeWidth = 0.0f;
        push.shapeMode = 0;

        float halfW = 0.5f, halfH = 0.5f;
        const bromath::Vec3& scl = node->scale();
        VkDescriptorSet matSet = VK_NULL_HANDLE;

        if (node->type() == SceneNode::Type::Shape) {
            auto* s = static_cast<ShapeNode*>(node);
            switch (s->shape()) {
            case ShapeNode::Shape::Rect:
            case ShapeNode::Shape::RoundRect:
                push.shapeMode = 0;
                halfW = 0.5f * s->width() * scl.x;
                halfH = 0.5f * s->height() * scl.y;
                break;
            case ShapeNode::Shape::Circle:
                push.shapeMode = 1;
                halfW = s->radius() * scl.x;
                halfH = s->radius() * scl.y;
                break;
            case ShapeNode::Shape::Ellipse:
                push.shapeMode = 1;
                halfW = s->radiusX() * scl.x;
                halfH = s->radiusY() * scl.y;
                break;
            default:
                push.shapeMode = 0;
                halfW = 0.5f * s->width() * scl.x;
                halfH = 0.5f * s->height() * scl.y;
                break;
            }
            const auto& fc = s->fillColor();
            push.color[0] = bromath::clinearToSrgb(fc.r);
            push.color[1] = bromath::clinearToSrgb(fc.g);
            push.color[2] = bromath::clinearToSrgb(fc.b);
            push.color[3] = s->hasFill() ? fc.a : 0.0f;

            const auto& sc = s->strokeColor();
            push.stroke[0] = bromath::clinearToSrgb(sc.r);
            push.stroke[1] = bromath::clinearToSrgb(sc.g);
            push.stroke[2] = bromath::clinearToSrgb(sc.b);
            push.stroke[3] = sc.a;

            float uvRef = std::max(halfW, halfH) * 2.0f;
            push.strokeWidth = (s->hasStroke() && uvRef > 0.0f) ? (s->strokeWidth() / uvRef) : 0.0f;
        } else if (node->type() == SceneNode::Type::Sprite) {
            auto* s = static_cast<SpriteNode*>(node);
            float worldW = s->width();
            float worldH = s->height();
            if (worldW <= 0.0f || worldH <= 0.0f) {
                float sx, sy, sw, sh;
                if (s->currentSheetRect(sx, sy, sw, sh)) {
                    if (worldW <= 0.0f) worldW = sw;
                    if (worldH <= 0.0f) worldH = sh;
                } else if (s->imageWidth() > 0 && s->imageHeight() > 0) {
                    if (worldW <= 0.0f) worldW = static_cast<float>(s->imageWidth());
                    if (worldH <= 0.0f) worldH = static_cast<float>(s->imageHeight());
                }
            }
            push.shapeMode = 4;
            halfW = 0.5f * worldW * scl.x;
            halfH = 0.5f * worldH * scl.y;
            push.color[3] = s->opacity();
            s->currentUvRect(push.uvMin[0], push.uvMin[1], push.uvMax[0], push.uvMax[1]);

            if (s->hasImage()) {
                matSet = uploadTexture(s, s->imageWidth(), s->imageHeight(), s->imagePixels().data());
            } else {
                push.color[3] = 0.0f;
            }
        } else if (node->type() == SceneNode::Type::Html) {
            auto* h = static_cast<HtmlNode*>(node);
            float ppu = h->pxPerUnit();
            if (ppu <= 0.0f) ppu = 100.0f;
            push.shapeMode = 2;
            halfW = 0.5f * (h->layoutWidth() / ppu) * scl.x;
            halfH = 0.5f * (h->layoutHeight() / ppu) * scl.y;
            push.color[3] = 1.0f;

            if (!h->pixels().empty() && h->textureWidth() > 0 && h->textureHeight() > 0) {
                matSet = uploadTexture(h, h->textureWidth(), h->textureHeight(), h->pixels().data());
            } else {
                push.color[3] = 0.0f;
            }
        } else {
            continue;
        }

        if (push.color[3] <= 0.0f && push.shapeMode != 2) continue;

        const bromath::Vec3 anchor = node->worldAnchor();
        if (renderer.cullingActive_) {
            float r = std::sqrt(halfW * halfW + halfH * halfH);
            if (!bromath::fintersects(renderer.cameraFrustum_, bromath::Sphere{anchor, r})) {
                stats.billboardsCulled++;
                continue;
            }
        }
        stats.billboardsDrawn++;

        push.anchor[0] = anchor.x;
        push.anchor[1] = anchor.y;
        push.anchor[2] = anchor.z;
        push.anchor[3] = 0.0f;

        bromath::Vec3 right = camRight;
        bromath::Vec3 up = camUp;
        if (node->billboardMode() == SceneNode::BillboardMode::YLock) {
            if (std::abs(camForward.y) < 0.99f) {
                up = {0.0f, 1.0f, 0.0f};
                bromath::Vec3 flatRight{camRight.x, 0.0f, camRight.z};
                float len = bromath::vlen(flatRight);
                if (len > 1e-5f) {
                    right = flatRight * (1.0f / len);
                }
            }
        }

        push.right[0] = right.x; push.right[1] = right.y; push.right[2] = right.z;
        push.up[0] = up.x; push.up[1] = up.y; push.up[2] = up.z;
        push.halfSize[0] = halfW;
        push.halfSize[1] = halfH;

        passBillboard_.draw(cmd, push, matSet);
        hasDrawnMeshes = true;
    }

    if (hasLightIcons) {
        for (auto& [id, node] : graph.nodes_) {
            if (!node->renderVisible() || node->type() != SceneNode::Type::Light) continue;
            auto* light = static_cast<LightNode*>(node.get());
            const auto& M = light->worldMatrix();
            bromath::Vec3 anchor{M.at(0, 3), M.at(1, 3), M.at(2, 3)};

            BillboardPushConstants push{};
            push.anchor[0] = anchor.x; push.anchor[1] = anchor.y; push.anchor[2] = anchor.z;
            push.right[0] = camRight.x; push.right[1] = camRight.y; push.right[2] = camRight.z;
            push.up[0] = camUp.x; push.up[1] = camUp.y; push.up[2] = camUp.z;

            const auto& lc = light->color();
            float lum = 0.299f * lc.x + 0.587f * lc.y + 0.114f * lc.z;
            float lift = lum < 0.2f ? 0.2f : 0.0f;
            push.color[0] = lc.x + lift;
            push.color[1] = lc.y + lift;
            push.color[2] = lc.z + lift;
            push.color[3] = 1.0f;

            float half = 0.22f;
            float strokeT = 0.12f;
            if (light->kind() == LightNode::Kind::Directional) {
                half = 0.30f; strokeT = 0.18f;
                push.stroke[0] = push.stroke[1] = push.stroke[2] = push.stroke[3] = 1.0f;
            } else if (light->kind() == LightNode::Kind::Point) {
                half = 0.22f; strokeT = 0.12f;
                push.stroke[0] = push.color[0] * 0.5f;
                push.stroke[1] = push.color[1] * 0.5f;
                push.stroke[2] = push.color[2] * 0.5f;
                push.stroke[3] = 1.0f;
            } else {
                half = 0.22f; strokeT = 0.28f;
                push.stroke[0] = std::min(push.color[0] * 0.8f, 1.0f);
                push.stroke[1] = std::min(push.color[1] * 0.8f, 1.0f);
                push.stroke[2] = std::min(push.color[2] * 0.8f, 1.0f);
                push.stroke[3] = 1.0f;
            }
            push.halfSize[0] = half; push.halfSize[1] = half;
            push.shapeMode = 3;
            push.strokeWidth = strokeT;
            push.uvMin[0] = 0.0f; push.uvMin[1] = 0.0f;
            push.uvMax[0] = 1.0f; push.uvMax[1] = 1.0f;

            passBillboard_.draw(cmd, push, VK_NULL_HANDLE);
            hasDrawnMeshes = true;
        }
    }
}

} // namespace bro::scene::vk

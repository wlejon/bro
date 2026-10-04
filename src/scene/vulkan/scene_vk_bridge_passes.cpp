#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/html_node.h"
#include "scene/sprite_node.h"
#include "scene/shape_node.h"
#include "scene/particles3d_node.h"
#include "scene/decal_node.h"
#include "scene/light_node.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/gaussian_splat_node.h"
#include "scene/custom_shader.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "util/log.h"
#include <bromath/color.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

void SceneVkBridge::prepareDynamicBuffers(SceneGraph& graph) {
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (mn->asSkinnedMesh()) {
                auto* sm = mn->asSkinnedMesh();
                if (!sm->currentMesh().empty()) {
                    uploadMesh(sm->currentMesh(), sm);
                    auto& dynBuf = getDynamicBuffers(sm);
                    size_t skinVc = sm->currentMesh().vertexCount();
                    size_t skinByteSize = skinVc * 24;
                    if (dynBuf.skinAttribCapacity < skinByteSize) {
                        allocator_.destroyBuffer(dynBuf.skinAttribBuffer);
                        std::vector<uint8_t> skinBytes(skinByteSize, 0);
                        const auto& j = sm->skinJoints();
                        const auto& w = sm->skinWeights();
                        for (size_t vi = 0; vi < skinVc; ++vi) {
                            uint16_t* dstJ = reinterpret_cast<uint16_t*>(skinBytes.data() + vi * 24);
                            float* dstW = reinterpret_cast<float*>(skinBytes.data() + vi * 24 + 8);
                            if (vi * 4 + 3 < j.size()) {
                                dstJ[0] = j[vi * 4 + 0]; dstJ[1] = j[vi * 4 + 1];
                                dstJ[2] = j[vi * 4 + 2]; dstJ[3] = j[vi * 4 + 3];
                            }
                            if (vi * 4 + 3 < w.size()) {
                                dstW[0] = w[vi * 4 + 0]; dstW[1] = w[vi * 4 + 1];
                                dstW[2] = w[vi * 4 + 2]; dstW[3] = w[vi * 4 + 3];
                            } else {
                                dstW[0] = 1.0f;
                            }
                        }
                        allocator_.createVertexBuffer(skinByteSize, skinBytes.data(), dynBuf.skinAttribBuffer);
                        dynBuf.skinAttribCapacity = skinByteSize;
                    }

                    if (!dynBuf.boneUbo.isValid()) {
                        allocator_.createUniformBuffer(256 * 16 * sizeof(float), dynBuf.boneUbo);
                        dynBuf.boneSet = dynamicDescPool_.allocate(passMesh_.bonePaletteLayout());
                        SceneVkDescriptorWriter writer;
                        writer.writeBuffer(0, dynBuf.boneUbo.buffer, 256 * 16 * sizeof(float));
                        writer.updateSet(device_.device(), dynBuf.boneSet);

                        dynBuf.boneSetShadow = dynamicDescPool_.allocate(passShadow_.bonePaletteLayout());
                        SceneVkDescriptorWriter writerShadow;
                        writerShadow.writeBuffer(0, dynBuf.boneUbo.buffer, 256 * 16 * sizeof(float));
                        writerShadow.updateSet(device_.device(), dynBuf.boneSetShadow);
                    }
                    std::vector<float> boneData(256 * 16, 0.0f);
                    for (int b = 0; b < 256; ++b) {
                        boneData[b * 16 + 0] = 1.0f;
                        boneData[b * 16 + 5] = 1.0f;
                        boneData[b * 16 + 10] = 1.0f;
                        boneData[b * 16 + 15] = 1.0f;
                    }
                    const auto& pal = sm->skinPalette();
                    if (!pal.empty()) {
                        size_t copyCount = std::min(pal.size(), boneData.size());
                        std::memcpy(boneData.data(), pal.data(), copyCount * sizeof(float));
                    }
                    allocator_.updateUniformBuffer(dynBuf.boneUbo, boneData.data(), boneData.size() * sizeof(float));
                }
            } else {
                if (!mn->currentMesh().empty()) {
                    uploadMesh(mn->currentMesh(), mn);
                }
            }
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<InstancedMeshNode*>(node.get());
            if (!im->mesh().empty() && im->instanceCount() > 0) {
                uploadMesh(im->mesh(), im);
                auto& dynBuf = getDynamicBuffers(im);
                size_t instByteSize = im->instanceCount() * 16 * sizeof(float);
                if (dynBuf.instanceCapacity < instByteSize) {
                    allocator_.destroyBuffer(dynBuf.instanceBuffer);
                    allocator_.createVertexBuffer(instByteSize, im->instanceData().data(), dynBuf.instanceBuffer);
                    dynBuf.instanceCapacity = instByteSize;
                } else {
                    allocator_.stageAndUploadBuffer(dynBuf.instanceBuffer.buffer, im->instanceData().data(), instByteSize);
                }
            }
        }
    }
}

void SceneVkBridge::renderShadowPass(VkCommandBuffer cmd, SceneGraph& graph,
                                     const SceneLightingUniforms& lightUniforms, CullStats& stats) {
    if (lightUniforms.numLights[2] <= 0.5f) return;

    passShadow_.beginCascade(cmd, device_, shadowTarget_, 0, lightUniforms.shadowCascadeProj);
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (!mn->castsShadow() || mn->currentMesh().empty()) continue;
            auto& meshBuf = uploadMesh(mn->currentMesh(), mn);
            if (mn->asSkinnedMesh()) {
                auto* sm = mn->asSkinnedMesh();
                auto& dynBuf = getDynamicBuffers(sm);
                SkinnedShadowCaster caster{};
                caster.vertexBuffer = meshBuf.vertexBuffer.buffer;
                caster.indexBuffer = meshBuf.indexBuffer.buffer;
                caster.indexCount = meshBuf.indexCount;
                caster.skinAttribBuffer = dynBuf.skinAttribBuffer.buffer;
                caster.bonePaletteSet = dynBuf.boneSetShadow;
                std::memcpy(caster.modelMatrix, sm->worldMatrix().data, sizeof(caster.modelMatrix));
                if (sm->hasCustomShader() && !sm->customShader()->vertexChunk.empty()) {
                    prepareCustomShadowShaderForNode(sm, sm->customShader(), true, sm->customShaderTextures(),
                                                     caster.customPipeline, caster.customSet);
                }
                passShadow_.drawSkinned(cmd, caster);
                if (stats.shadowTilesTotal == 0) stats.shadowDrawn++;
            } else {
                ShadowCaster caster{};
                caster.vertexBuffer = meshBuf.vertexBuffer.buffer;
                caster.indexBuffer = meshBuf.indexBuffer.buffer;
                caster.indexCount = meshBuf.indexCount;
                std::memcpy(caster.modelMatrix, mn->worldMatrix().data, sizeof(caster.modelMatrix));
                if (mn->hasCustomShader() && !mn->customShader()->vertexChunk.empty()) {
                    prepareCustomShadowShaderForNode(mn, mn->customShader(), false, mn->customShaderTextures(),
                                                     caster.customPipeline, caster.customSet);
                }
                passShadow_.drawStatic(cmd, caster);
                if (stats.shadowTilesTotal == 0) stats.shadowDrawn++;
            }
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<InstancedMeshNode*>(node.get());
            if (!im->castsShadow() || im->mesh().empty() || im->instanceCount() == 0) continue;
            auto& meshBuf = uploadMesh(im->mesh(), im);
            auto& dynBuf = getDynamicBuffers(im);
            InstancedShadowCaster caster{};
            caster.vertexBuffer = meshBuf.vertexBuffer.buffer;
            caster.indexBuffer = meshBuf.indexBuffer.buffer;
            caster.indexCount = meshBuf.indexCount;
            caster.instanceBuffer = dynBuf.instanceBuffer.buffer;
            caster.instanceCount = static_cast<uint32_t>(im->instanceCount());
            std::memcpy(caster.modelMatrix, im->worldMatrix().data, sizeof(caster.modelMatrix));
            passShadow_.drawInstanced(cmd, caster);
            if (stats.shadowTilesTotal == 0) stats.shadowDrawn++;
        }
    }
    passShadow_.endCascade(cmd, device_, shadowTarget_);
    shadowTarget_.transitionToShaderRead(cmd, allocator_);
}

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

void SceneVkBridge::prepareCustomShaderForNode(const void* key, const CustomShaderState* cs, uint32_t target, bool translucent,
                                                std::vector<MeshNode::UserTexture>& userTextures,
                                                VkPipeline& outPipeline, VkDescriptorSet& outSet) {
    if (!cs) return;

    std::string pKey = cs->key + "_" + std::to_string(target) + "_" + (translucent ? "1" : "0");
    auto it = customMeshPipelines_.find(pKey);
    if (it == customMeshPipelines_.end()) {
        auto csTarget = (target == 1) ? SceneRenderer::CustomShaderTarget::Instanced
                                      : (target == 2 ? SceneRenderer::CustomShaderTarget::Skinned
                                                     : SceneRenderer::CustomShaderTarget::Static);
        VkShaderModule vs = VK_NULL_HANDLE;
        VkShaderModule fs = VK_NULL_HANDLE;
        std::vector<std::string> samplerNames;
        std::string errOut;
        if (!SceneVkCustomShader::compileCustomShaderModules(device_.device(), csTarget,
                                                             cs->vertexChunk, cs->fragmentChunk,
                                                             vs, fs, samplerNames, errOut)) {
            LOG_ERROR("SceneVkBridge: Failed to compile custom shader: %s", errOut.c_str());
            return;
        }
        VkPipeline pipe = passMesh_.createCustomPipeline(device_.device(), vs, fs, target, translucent);
        SceneVkShaderModule::destroy(device_.device(), vs);
        SceneVkShaderModule::destroy(device_.device(), fs);
        if (pipe == VK_NULL_HANDLE) {
            LOG_ERROR("SceneVkBridge: Failed to create custom pipeline");
            return;
        }
        uint32_t uboSize = 0;
        auto offsets = SceneVkCustomShader::parseUniformOffsets(cs->vertexChunk + "\n" + cs->fragmentChunk, uboSize);
        if (uboSize == 0) uboSize = 16;
        uboSize = (uboSize + 15) & ~15;

        CustomPipelineEntry entry;
        entry.pipeline = pipe;
        entry.samplerNames = std::move(samplerNames);
        entry.uniformOffsets = std::move(offsets);
        entry.uboSize = uboSize;
        it = customMeshPipelines_.emplace(pKey, std::move(entry)).first;
    }
    const auto& entry = it->second;
    outPipeline = entry.pipeline;

    auto& nodeBuf = customNodeBufferCache_[key];
    if (!nodeBuf.ubo.isValid() || nodeBuf.ubo.size < entry.uboSize) {
        if (nodeBuf.ubo.isValid()) {
            allocator_.destroyBuffer(nodeBuf.ubo);
        }
        if (!allocator_.createUniformBuffer(entry.uboSize, nodeBuf.ubo)) {
            LOG_ERROR("SceneVkBridge: Failed to allocate uniform buffer for custom shader (%u bytes)", entry.uboSize);
            return;
        }
        nodeBuf.descSet = dynamicDescPool_.allocate(passMesh_.customLayout());
        if (!nodeBuf.descSet) {
            LOG_ERROR("SceneVkBridge: Failed to allocate descriptor set for custom shader");
            return;
        }
    }

    std::vector<uint8_t> uboData(entry.uboSize, 0);
    for (const auto& [name, offset] : entry.uniformOffsets) {
        for (const auto& u : cs->uniforms) {
            if (u.name == name) {
                uint32_t bytesToCopy = static_cast<uint32_t>(std::clamp(u.comps, 1, 4) * sizeof(float));
                if (offset + bytesToCopy <= entry.uboSize) {
                    std::memcpy(uboData.data() + offset, u.v, bytesToCopy);
                }
                break;
            }
        }
    }
    allocator_.updateUniformBuffer(nodeBuf.ubo, uboData.data(), entry.uboSize);

    for (auto& ut : userTextures) {
        if (ut.w <= 0 || ut.h <= 0) continue;
        std::string tKey = std::to_string(reinterpret_cast<uintptr_t>(key)) + "_" + ut.name;
        auto& cachedTex = userTextureCache_[tKey];

        VkFormat fmt = VK_FORMAT_R32_SFLOAT;
        if (ut.channels == 2) fmt = VK_FORMAT_R32G32_SFLOAT;
        else if (ut.channels >= 3) fmt = VK_FORMAT_R32G32B32A32_SFLOAT;

        if (ut.dirty || !cachedTex.image.isValid() || cachedTex.width != ut.w || cachedTex.height != ut.h) {
            if (cachedTex.image.isValid() && cachedTex.owned) {
                allocator_.destroyImage(cachedTex.image);
            }
            cachedTex.image = {};
            cachedTex.owned = true;
            cachedTex.width = ut.w;
            cachedTex.height = ut.h;

            const float* srcPixels = ut.data.data();
            std::vector<float> expanded;
            if (ut.channels == 3 && !ut.data.empty()) {
                expanded.resize((size_t)ut.w * ut.h * 4);
                for (size_t p = 0; p < (size_t)ut.w * ut.h; ++p) {
                    expanded[p * 4 + 0] = ut.data[p * 3 + 0];
                    expanded[p * 4 + 1] = ut.data[p * 3 + 1];
                    expanded[p * 4 + 2] = ut.data[p * 3 + 2];
                    expanded[p * 4 + 3] = 1.0f;
                }
                srcPixels = expanded.data();
            }

            TextureDesc tDesc{};
            tDesc.width = static_cast<uint32_t>(ut.w);
            tDesc.height = static_cast<uint32_t>(ut.h);
            tDesc.format = fmt;
            tDesc.generateMipmaps = ut.mipmap;
            tDesc.minFilter = VK_FILTER_LINEAR;
            tDesc.magFilter = VK_FILTER_LINEAR;
            tDesc.mipmapMode = ut.mipmap ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            tDesc.addressModeU = ut.repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            tDesc.addressModeV = (ut.repeat && !ut.clampT) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            tDesc.enableAnisotropy = false;
            if (!allocator_.createTexture2D(srcPixels, tDesc, cachedTex.image)) {
                LOG_ERROR("SceneVkBridge: Failed to allocate custom user texture %s (%dx%d)", ut.name.c_str(), ut.w, ut.h);
            }
            ut.dirty = false;
        }

        if (!ut.subUpdates.empty() && cachedTex.image.isValid()) {
            uint32_t bpp = (fmt == VK_FORMAT_R32_SFLOAT) ? 4 : ((fmt == VK_FORMAT_R32G32_SFLOAT) ? 8 : 16);
            for (const auto& sub : ut.subUpdates) {
                if (sub.w <= 0 || sub.h <= 0 || sub.data.empty()) continue;
                VkDeviceSize subSize = static_cast<VkDeviceSize>(sub.w) * sub.h * bpp;
                SceneVkBuffer staging;
                if (allocator_.createBuffer(subSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
                    if (staging.mappedData) {
                        std::memcpy(staging.mappedData, sub.data.data(), subSize);
                    } else {
                        void* m = nullptr;
                        vkMapMemory(device_.device(), staging.memory, staging.offset, subSize, 0, &m);
                        std::memcpy(m, sub.data.data(), subSize);
                        vkUnmapMemory(device_.device(), staging.memory);
                    }
                    device_.executeImmediate([&](VkCommandBuffer copyCmd) {
                        allocator_.transitionImageLayout(copyCmd, cachedTex.image.image, fmt,
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                         cachedTex.image.mipLevels, 0);
                        VkBufferImageCopy region{};
                        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        region.imageSubresource.mipLevel = 0;
                        region.imageSubresource.baseArrayLayer = 0;
                        region.imageSubresource.layerCount = 1;
                        region.imageOffset = {sub.x, sub.y, 0};
                        region.imageExtent = {static_cast<uint32_t>(sub.w), static_cast<uint32_t>(sub.h), 1};
                        vkCmdCopyBufferToImage(copyCmd, staging.buffer, cachedTex.image.image,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                        if (ut.mipmap && cachedTex.image.mipLevels > 1) {
                            allocator_.generateMipmaps(copyCmd, cachedTex.image.image, fmt,
                                                       cachedTex.image.width, cachedTex.image.height, cachedTex.image.mipLevels);
                        } else {
                            allocator_.transitionImageLayout(copyCmd, cachedTex.image.image, fmt,
                                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                             cachedTex.image.mipLevels, 0);
                        }
                    });
                    allocator_.destroyBuffer(staging);
                }
            }
            ut.subUpdates.clear();
        }
    }

    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, nodeBuf.ubo.buffer, entry.uboSize);

    for (uint32_t i = 1; i <= 8; ++i) {
        VkImageView v = passMesh_.dummyWhiteView();
        VkSampler s = passMesh_.defaultSampler();
        if (i - 1 < entry.samplerNames.size()) {
            const std::string& sName = entry.samplerNames[i - 1];
            std::string tKey = std::to_string(reinterpret_cast<uintptr_t>(key)) + "_" + sName;
            auto itTex = userTextureCache_.find(tKey);
            if (itTex != userTextureCache_.end() && itTex->second.image.isValid()) {
                v = itTex->second.image.view;
                s = itTex->second.image.sampler;
            }
        }
        writer.writeImage(i, v, s);
    }
    writer.updateSet(device_.device(), nodeBuf.descSet);
    outSet = nodeBuf.descSet;
}

void SceneVkBridge::prepareCustomShadowShaderForNode(const void* key, const CustomShaderState* cs, bool isSkinned,
                                                    std::vector<MeshNode::UserTexture>& userTextures,
                                                    VkPipeline& outPipeline, VkDescriptorSet& outSet) {
    if (!cs || cs->vertexChunk.empty()) return;
    std::string sKey = cs->key + "_" + (isSkinned ? "skin" : "static");
    auto it = customShadowPipelines_.find(sKey);
    if (it == customShadowPipelines_.end()) {
        VkShaderModule vs = VK_NULL_HANDLE;
        std::string errOut;
        if (!SceneVkCustomShader::compileCustomShadowShaderModule(device_.device(), isSkinned, cs->vertexChunk, vs, errOut)) {
            LOG_ERROR("SceneVkBridge: Failed to compile custom shadow shader: %s", errOut.c_str());
            return;
        }
        VkPipeline pipe = passShadow_.createCustomPipeline(device_.device(), vs, isSkinned);
        SceneVkShaderModule::destroy(device_.device(), vs);
        if (pipe == VK_NULL_HANDLE) {
            LOG_ERROR("SceneVkBridge: Failed to create custom shadow pipeline");
            return;
        }
        it = customShadowPipelines_.emplace(sKey, pipe).first;
    }
    outPipeline = it->second;

    VkPipeline dummyPipe = VK_NULL_HANDLE;
    prepareCustomShaderForNode(key, cs, isSkinned ? 2 : 0, false, userTextures, dummyPipe, outSet);
}

void SceneVkBridge::renderGaussianSplatPass(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                            CullStats& stats, const float* view, const float* proj,
                                            const float eye[3], uint32_t width, uint32_t height) {
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::GaussianSplat) continue;
        if (renderer.cameraCulled(node.get())) {
            stats.splatCulled++;
            continue;
        }
        stats.splatDrawn++;
        auto* gsn = static_cast<GaussianSplatNode*>(node.get());
        if (gsn->splatCount() > 0) {
            passGaussianSplat_.renderNode(cmd, device_, allocator_, frameDescPool_,
                                         gsn, view, proj, eye, width, height);
            hasMeshContent_ = true;
        }
    }
}

void SceneVkBridge::renderPostProcessing(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                         uint32_t width, uint32_t height) {
    const SceneVkImage* hdrInput = &hdrTarget_.colorImage();

    // 1. Depth of Field pass
    if (renderer.depthOfFieldEnabled() && dofHdrImage_.isValid()) {
        allocator_.transitionImageLayout(cmd, dofHdrImage_.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         dofHdrImage_.currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        dofHdrImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        DoFParams dofParams{};
        dofParams.focusDistance = renderer.depthOfFieldFocusDistance();
        dofParams.focusRange = renderer.depthOfFieldFocusRange();
        dofParams.maxBlur = renderer.depthOfFieldMaxBlur();
        dofParams.nearPlane = graph.cameraNearZ_;
        dofParams.farPlane = graph.cameraFarZ_;
        dofParams.isPerspective = graph.cameraIsPerspective_;

        passDoF_.render(cmd, device_, allocator_, frameDescPool_,
                        *hdrInput, depthCopyImage_, dofHdrImage_.view,
                        width, height, dofParams);

        allocator_.transitionImageLayout(cmd, dofHdrImage_.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dofHdrImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        hdrInput = &dofHdrImage_;
    }

    // 2. PostFx Tonemap / Bloom / FXAA pass
    PostFxParams postFx{};
    postFx.exposure = renderer.exposure();
    postFx.gamma = renderer.gamma();
    postFx.tonemapMode = static_cast<TonemapMode>(renderer.toneMap());
    postFx.enableBloom = renderer.bloomEnabled();
    postFx.bloomIntensity = renderer.bloomIntensity();
    postFx.enableFxaa = renderer.fxaaEnabled();

    bool useLut = renderer.hasColorLUT() && renderer.colorLUTAmount() > 0.0f && postLdrImage_.isValid();

    if (useLut) {
        if (renderer.isColorLUTDirty()) {
            passColorLut_.updateLut(device_, allocator_, renderer.colorLUTSize(), renderer.colorLUTVoxels().data());
            renderer.setColorLUTClean();
        }

        allocator_.transitionImageLayout(cmd, postLdrImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                         postLdrImage_.currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        postLdrImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        passPostFx_.render(cmd, device_, allocator_,
                          *hdrInput, postLdrImage_.view,
                          VK_FORMAT_R8G8B8A8_UNORM, width, height, postFx);

        allocator_.transitionImageLayout(cmd, postLdrImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        postLdrImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                         ldrPresentationImage_.currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ldrPresentationImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        passColorLut_.render(cmd, device_, allocator_, frameDescPool_,
                             postLdrImage_, ldrPresentationImage_.view,
                             VK_FORMAT_R8G8B8A8_UNORM, width, height, renderer.colorLUTAmount());
    } else {
        allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                         ldrPresentationImage_.currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ldrPresentationImage_.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        passPostFx_.render(cmd, device_, allocator_,
                          *hdrInput, ldrPresentationImage_.view,
                          VK_FORMAT_R8G8B8A8_UNORM, width, height, postFx);
    }
}

} // namespace bro::scene::vk

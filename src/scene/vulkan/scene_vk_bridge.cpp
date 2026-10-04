#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/light_node.h"
#include "scene/html_node.h"
#include "scene/sprite_node.h"
#include "scene/shape_node.h"
#include "scene/particles3d_node.h"
#include "scene/decal_node.h"
#include "scene/depth_policy.h"
#include "util/log.h"
#include <bromath/color.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

SceneVkBridge::SceneVkBridge(render::VulkanContext& context)
    : context_(context), device_(context), allocator_(device_) {}

SceneVkBridge::~SceneVkBridge() {
    device_.waitIdle();

    for (auto& [k, buf] : meshCache_) {
        allocator_.destroyBuffer(buf.vertexBuffer);
        allocator_.destroyBuffer(buf.indexBuffer);
    }
    meshCache_.clear();

    for (auto& [k, dyn] : dynamicBufferCache_) {
        allocator_.destroyBuffer(dyn.skinAttribBuffer);
    }
    dynamicBufferCache_.clear();

    for (auto& [k, tex] : textureCache_) {
        if (tex.owned) {
            allocator_.destroyImage(tex.image);
        }
    }
    textureCache_.clear();

    for (auto& [k, pipe] : customMeshPipelines_) {
        if (pipe.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.device(), pipe.pipeline, nullptr);
        }
    }
    customMeshPipelines_.clear();

    for (auto& [k, pipe] : customShadowPipelines_) {
        if (pipe != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.device(), pipe, nullptr);
        }
    }
    customShadowPipelines_.clear();

    for (auto& [k, tex] : userTextureCache_) {
        if (tex.owned) {
            allocator_.destroyImage(tex.image);
        }
    }
    userTextureCache_.clear();

    allocator_.destroyBuffer(readbackBuffer_);
    allocator_.destroyImage(ssrColorSnapshot_);
    allocator_.destroyImage(dofHdrImage_);
    allocator_.destroyImage(postLdrImage_);
    allocator_.destroyImage(ldrPresentationImage_);
    allocator_.destroyImage(depthCopyImage_);
    allocator_.destroyImage(dummyShadeMap_);
    allocator_.destroyImage(shadeMapImage_);

    hdrTarget_.cleanup(allocator_);
    shadowTarget_.cleanup(allocator_);

    passColorLut_.cleanup(device_, allocator_);
    passSSAO_.cleanup(device_, allocator_);
    passSSR_.cleanup(device_, allocator_);
    passDoF_.cleanup(device_, allocator_);
    passGaussianSplat_.cleanup(device_, allocator_);
    passTerrain_.cleanup(device_, allocator_);
    passReflectionProbe_.cleanup(device_, allocator_);
    passBillboard_.cleanup(device_, allocator_);
    passParticles_.cleanup(device_, allocator_);
    passDecal_.cleanup(device_, allocator_);
    passPostFx_.cleanup(device_, allocator_);
    passMesh_.cleanup(device_, allocator_);
    passEnv_.cleanup(device_, allocator_);
    passShadow_.cleanup(device_);
    device_.shutdown();
}

bool SceneVkBridge::init() {
    if (!device_.init()) {
        LOG_ERROR("SceneVkBridge: Failed initializing SceneVkDevice");
        return false;
    }

    if (!passShadow_.init(device_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassShadow");
        return false;
    }

    if (!passEnv_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassEnvironment");
        return false;
    }

    if (!passMesh_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassMesh");
        return false;
    }

    if (!passBillboard_.init(device_, allocator_, passMesh_.cameraLayout(), passMesh_.materialLayout(), passMesh_.defaultMaterialSet())) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassBillboard");
        return false;
    }

    if (!passParticles_.init(device_, allocator_, passMesh_.cameraLayout())) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassParticles");
        return false;
    }

    if (!passDecal_.init(device_, allocator_, passMesh_.cameraLayout(), passMesh_.lightingLayout())) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassDecal");
        return false;
    }

    if (!passReflectionProbe_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassReflectionProbe");
        return false;
    }

    if (!passTerrain_.init(device_, allocator_, passMesh_.cameraLayout(), passMesh_.lightingLayout())) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassTerrain");
        return false;
    }

    if (!passColorLut_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassColorLut");
        return false;
    }

    if (!passSSAO_.init(device_, allocator_, 1, 1)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassSSAO");
        return false;
    }

    if (!passSSR_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassSSR");
        return false;
    }

    if (!passDoF_.init(device_, allocator_, 1, 1)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassDoF");
        return false;
    }

    if (!passGaussianSplat_.init(device_, allocator_)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassGaussianSplat");
        return false;
    }

    // Sized to the target by its first render.
    if (!passPostFx_.init(device_, allocator_, 1, 1)) {
        LOG_ERROR("SceneVkBridge: Failed initializing PassPostFx");
        return false;
    }

    uint8_t whitePixel[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc{};
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    whiteDesc.generateMipmaps = false;
    if (!allocator_.createTexture2D(whitePixel, whiteDesc, dummyShadeMap_)) {
        LOG_ERROR("SceneVkBridge: Failed creating dummy shade map");
        return false;
    }

    return true;
}


void SceneVkBridge::render3D(SceneGraph& graph, SceneRenderer& renderer) {
    uint32_t width = static_cast<uint32_t>(renderer.targetWidth());
    uint32_t height = static_cast<uint32_t>(renderer.targetHeight());
    if (width == 0 || height == 0) return;

    VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
    if (renderer.msaaSamples() >= 8) sampleCount = VK_SAMPLE_COUNT_8_BIT;
    else if (renderer.msaaSamples() >= 4) sampleCount = VK_SAMPLE_COUNT_4_BIT;
    else if (renderer.msaaSamples() >= 2) sampleCount = VK_SAMPLE_COUNT_2_BIT;

    if (!ensureTargets(width, height, sampleCount)) return;

    renderer.setCullingActive(renderer.frustumCullingEnabled());
    if (renderer.cullingActive_) {
        renderer.cameraFrustum_ = makeFrustum(
            bromath::mmul(graph.projectionMatrix(), graph.viewMatrix()));
    }

    ++renderSerial_;
    const bool recordReadbackInline = readSinceRender_;
    readSinceRender_ = false;

    const SceneCameraUniforms camUniforms = buildCameraUniforms(graph, renderer, width, height);
    SceneLightingUniforms lightUniforms = buildLightingUniforms(graph, renderer);

    VkCommandBuffer cmd = device_.beginFrame();

    // Probes capture first (their faces have their own camera and lighting);
    // the scene's lighting set then points at the active probe.
    passReflectionProbe_.updateProbes(cmd, graph, renderer, passMesh_, allocator_, device_, *this);
    finishLightingUniforms(graph, lightUniforms);
    writeFrameSets(camUniforms, lightUniforms);

    CullStats stats = renderer.cullStats();
    bool hasDrawnMeshes = false;

    prepareDynamicBuffers(graph);
    renderShadowPass(cmd, graph, lightUniforms, stats);

    // HDR mesh pass
    hdrTarget_.beginRendering(cmd, device_,
                             VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                             {{0.0f, 0.0f, 0.0f, 0.0f}},
                             VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                             0.0f);

    if (renderer.atmosphere().enabled || !renderer.envPath().empty()) {
        EnvironmentParams envParams{};
        bromath::Mat4 flippedVP;
        std::memcpy(flippedVP.data, camUniforms.viewProj, sizeof(camUniforms.viewProj));
        bromath::Mat4 invVP = bromath::minverse(flippedVP);
        std::memcpy(envParams.invViewProj, invVP.data, sizeof(envParams.invViewProj));
        envParams.sunDirection[0] = lightUniforms.sunDirection[0];
        envParams.sunDirection[1] = lightUniforms.sunDirection[1];
        envParams.sunDirection[2] = lightUniforms.sunDirection[2];
        envParams.sunIntensity = lightUniforms.sunColor[3];
        envParams.skyIntensity = renderer.environmentIntensity();
        passEnv_.render(cmd, width, height, envParams);
    }

    passMesh_.begin(cmd, cameraSet_, lightingSet_, width, height);

    struct TranslucentDraw {
        enum class Type { Static, Instanced, Skinned } type;
        MeshDrawCall drawStatic;
        InstancedMeshDrawCall drawInstanced;
        SkinnedMeshDrawCall drawSkinned;
        float depth = 0.0f;
    };
    std::vector<TranslucentDraw> translucentDraws;
    const bromath::Vec3 viewFwd{-graph.viewMatrix().at(2, 0),
                                -graph.viewMatrix().at(2, 1),
                                -graph.viewMatrix().at(2, 2)};
    auto getDepth = [&](SceneNode* n) {
        bromath::Vec3 c;
        if (auto wbOpt = renderer.nodeWorldBounds(n)) {
            c = (wbOpt->min + wbOpt->max) * 0.5f;
        } else {
            const bromath::Mat4& w = n->worldMatrix();
            c = bromath::Vec3{w.at(0, 3), w.at(1, 3), w.at(2, 3)};
        }
        return bromath::vdot(c - graph.cameraEye(), viewFwd);
    };

    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;

        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (mn->name() == "clipmapTerrain" && mn->hasCustomShader()) {
                if (renderer.cameraCulled(mn)) {
                    stats.meshCulled++;
                    continue;
                }
                stats.meshDrawn++;
                hasDrawnMeshes = true;
                if (!mn->currentMesh().empty()) {
                    auto& meshBuf = uploadMesh(mn->currentMesh(), mn);
                    passTerrain_.render(cmd, mn, cameraSet_, lightingSet_, width, height,
                                        meshBuf.vertexBuffer.buffer, meshBuf.indexBuffer.buffer, meshBuf.indexCount);
                }
                continue;
            }
            if (mn->asSkinnedMesh()) {
                auto* sm = mn->asSkinnedMesh();
                if (renderer.cameraCulled(sm)) {
                    stats.meshCulled++;
                    continue;
                }
                stats.meshDrawn++;
                hasDrawnMeshes = true;
                if (!sm->currentMesh().empty()) {
                    auto& meshBuf = uploadMesh(sm->currentMesh(), sm);
                    auto& dynBuf = getDynamicBuffers(sm);

                    SkinnedMeshDrawCall draw{};
                    draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
                    draw.indexBuffer = meshBuf.indexBuffer.buffer;
                    draw.indexCount = meshBuf.indexCount;
                    draw.skinAttribBuffer = dynBuf.skinAttribBuffer.buffer;
                    draw.bonePaletteSet = dynBuf.boneSet;
                    std::memcpy(draw.modelMatrix, sm->worldMatrix().data, sizeof(draw.modelMatrix));
                    std::memcpy(draw.baseColor, sm->color(), sizeof(draw.baseColor));
                    std::memcpy(draw.emissiveColor, sm->emissiveColor(), sizeof(draw.emissiveColor));
                    draw.emissiveIntensity = sm->emissive();
                    draw.metallic = sm->metallic();
                    draw.roughness = sm->roughness();
                    draw.alphaCutoff = sm->alphaCutoff();
                    draw.flags = 0;
                    if (renderer.ssrEnabled()) draw.flags |= 64u;
                    if (sm->hasCustomShader()) {
                        prepareCustomShaderForNode(sm, sm->customShader(), 2, sm->color()[3] < 1.0f,
                                                   sm->customShaderTextures(), draw.customPipeline, draw.customSet);
                    } else {
                        if (sm->effectiveUnlit()) draw.flags |= 16u;
                    }
                    if (sm->shadeMap()) draw.flags |= 32u;

                    if (sm->color()[3] < 1.0f) {
                        translucentDraws.push_back({TranslucentDraw::Type::Skinned, {}, {}, draw, getDepth(sm)});
                    } else {
                        passMesh_.drawSkinned(cmd, draw);
                    }
                }
            } else {
                if (renderer.cameraCulled(mn)) {
                    stats.meshCulled++;
                    continue;
                }
                stats.meshDrawn++;
                hasDrawnMeshes = true;
                if (!mn->currentMesh().empty()) {
                    auto& meshBuf = uploadMesh(mn->currentMesh(), mn);
                    MeshDrawCall draw{};
                    draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
                    draw.indexBuffer = meshBuf.indexBuffer.buffer;
                    draw.indexCount = meshBuf.indexCount;
                    std::memcpy(draw.modelMatrix, mn->worldMatrix().data, sizeof(draw.modelMatrix));
                    std::memcpy(draw.baseColor, mn->color(), sizeof(draw.baseColor));
                    std::memcpy(draw.emissiveColor, mn->emissiveColor(), sizeof(draw.emissiveColor));
                    draw.emissiveIntensity = mn->emissive();
                    draw.metallic = mn->metallic();
                    draw.roughness = mn->roughness();
                    draw.alphaCutoff = mn->alphaCutoff();
                    draw.flags = 0;
                    if (renderer.ssrEnabled()) draw.flags |= 64u;
                    if (mn->hasCustomShader()) {
                        prepareCustomShaderForNode(mn, mn->customShader(), 0, mn->color()[3] < 1.0f,
                                                   mn->customShaderTextures(), draw.customPipeline, draw.customSet);
                    } else {
                        if (mn->effectiveUnlit()) draw.flags |= 16u;
                    }
                    if (mn->shadeMap()) draw.flags |= 32u;

                    const auto& pTex = mn->pendingBaseTexture();
                    if (pTex.w > 0 && pTex.h > 0 && !pTex.data.empty()) {
                        draw.materialSet = uploadTexture(mn, pTex.w, pTex.h, pTex.data.data());
                        if (draw.materialSet != VK_NULL_HANDLE) draw.flags |= 1u;
                    } else if (mn->hasExternalBaseColorTexture()) {
                        SceneGraph* extGraph = mn->externalSceneGraph();
                        if (extGraph && extGraph != &graph) {
                            auto* extBridge = extGraph->renderer().vkBridge();
                            if (extBridge && extBridge->ldrImage() != VK_NULL_HANDLE && extBridge->hasMeshContent()) {
                                VkDescriptorSet extSet = uploadExternalSceneTexture(extBridge, mn);
                                if (extSet != VK_NULL_HANDLE) {
                                    draw.materialSet = extSet;
                                    draw.flags |= 1u;
                                }
                            }
                        }
                    }
                    if (mn->color()[3] < 1.0f) {
                        translucentDraws.push_back({TranslucentDraw::Type::Static, draw, {}, {}, getDepth(mn)});
                    } else {
                        passMesh_.drawStatic(cmd, draw);
                    }
                }
            }
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<InstancedMeshNode*>(node.get());
            if (renderer.cameraCulled(im)) {
                stats.instancedCulled++;
                continue;
            }
            stats.instancedDrawn++;
            hasDrawnMeshes = true;
            if (!im->mesh().empty() && im->instanceCount() > 0) {
                auto& meshBuf = uploadMesh(im->mesh(), im);
                auto& dynBuf = getDynamicBuffers(im);

                InstancedMeshDrawCall draw{};
                draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
                draw.indexBuffer = meshBuf.indexBuffer.buffer;
                draw.indexCount = meshBuf.indexCount;
                if (!dynBuf.instances) continue;
                draw.instanceBuffer = dynBuf.instances.buffer;
                draw.instanceOffset = dynBuf.instances.offset;
                draw.instanceCount = static_cast<uint32_t>(im->instanceCount());
                std::memcpy(draw.modelMatrix, im->worldMatrix().data, sizeof(draw.modelMatrix));
                std::memcpy(draw.baseColor, im->color(), sizeof(draw.baseColor));
                std::memcpy(draw.emissiveColor, im->emissiveColor(), sizeof(draw.emissiveColor));
                draw.emissiveIntensity = im->emissive();
                draw.metallic = im->metallic();
                draw.roughness = im->roughness();
                draw.alphaCutoff = im->alphaCutoff();
                draw.flags = 0;
                if (renderer.ssrEnabled()) draw.flags |= 64u;
                if (im->hasCustomShader()) {
                    static std::vector<MeshNode::UserTexture> sEmptyTextures;
                    prepareCustomShaderForNode(im, im->customShader(), 1, im->color()[3] < 1.0f,
                                               sEmptyTextures, draw.customPipeline, draw.customSet);
                } else {
                    if (im->effectiveUnlit()) draw.flags |= 16u;
                }
                if (im->shadeMap()) draw.flags |= 32u;

                if (im->color()[3] < 1.0f) {
                    translucentDraws.push_back({TranslucentDraw::Type::Instanced, {}, draw, {}, getDepth(im)});
                } else {
                    passMesh_.drawInstanced(cmd, draw);
                }
            }
        }
    }

    // Depth snapshot for decals and soft particles
    hdrTarget_.endRendering(cmd, device_);

    allocator_.transitionImageLayout(cmd, hdrTarget_.depthImage().image, VK_FORMAT_D32_SFLOAT,
                                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);

    allocator_.transitionImageLayout(cmd, depthCopyImage_.image, VK_FORMAT_D32_SFLOAT,
                                     depthCopyImage_.currentLayout,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkImageCopy depthCopy{};
    depthCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthCopy.srcSubresource.layerCount = 1;
    depthCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthCopy.dstSubresource.layerCount = 1;
    depthCopy.extent = {width, height, 1};
    vkCmdCopyImage(cmd,
                   hdrTarget_.depthImage().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   depthCopyImage_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &depthCopy);

    allocator_.transitionImageLayout(cmd, hdrTarget_.depthImage().image, VK_FORMAT_D32_SFLOAT,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);

    allocator_.transitionImageLayout(cmd, depthCopyImage_.image, VK_FORMAT_D32_SFLOAT,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);
    depthCopyImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (renderer.ssrEnabled() && ssrColorSnapshot_.isValid()) {
        allocator_.transitionImageLayout(cmd, hdrTarget_.colorImage().image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        allocator_.transitionImageLayout(cmd, ssrColorSnapshot_.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         ssrColorSnapshot_.currentLayout,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageCopy colorCopy{};
        colorCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorCopy.srcSubresource.layerCount = 1;
        colorCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorCopy.dstSubresource.layerCount = 1;
        colorCopy.extent = {width, height, 1};
        vkCmdCopyImage(cmd,
                       hdrTarget_.colorImage().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       ssrColorSnapshot_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &colorCopy);

        allocator_.transitionImageLayout(cmd, hdrTarget_.colorImage().image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        allocator_.transitionImageLayout(cmd, ssrColorSnapshot_.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ssrColorSnapshot_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        SSRParams ssrParams{};
        ssrParams.isPerspective = graph.cameraIsPerspective_;
        ssrParams.maxDistance = renderer.ssrMaxDistance();
        ssrParams.steps = renderer.ssrSteps();
        ssrParams.thickness = renderer.ssrThickness();
        ssrParams.intensity = renderer.ssrIntensity();
        ssrParams.edgeFade = renderer.ssrEdgeFade();

        passSSR_.render(cmd, device_, allocator_,
                        ssrColorSnapshot_, depthCopyImage_, hdrTarget_.colorImage().view,
                        width, height, camUniforms.proj, camUniforms.invProj, ssrParams);
    }

    hdrTarget_.beginRendering(cmd, device_,
                             VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                             {{0.0f, 0.0f, 0.0f, 0.0f}},
                             VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                             0.0f);

    // Decals Pass
    renderDecalsPass(cmd, graph, renderer, stats, hasDrawnMeshes);

    if (!translucentDraws.empty()) {
        passMesh_.begin(cmd, cameraSet_, lightingSet_, width, height);
        std::stable_sort(translucentDraws.begin(), translucentDraws.end(),
                         [](const TranslucentDraw& a, const TranslucentDraw& b) {
                             return a.depth > b.depth;
                         });
        for (const auto& td : translucentDraws) {
            if (td.type == TranslucentDraw::Type::Static) {
                passMesh_.drawStaticTranslucent(cmd, td.drawStatic);
            } else if (td.type == TranslucentDraw::Type::Instanced) {
                passMesh_.drawInstancedTranslucent(cmd, td.drawInstanced);
            } else if (td.type == TranslucentDraw::Type::Skinned) {
                passMesh_.drawSkinnedTranslucent(cmd, td.drawSkinned);
            }
        }
    }

    // Particles3D Pass
    renderParticlesPass(cmd, graph, renderer, stats, hasDrawnMeshes);

    // Billboard & WorldQuad Pass
    renderBillboardsPass(cmd, graph, renderer, stats, hasDrawnMeshes);

    // Gaussian Splatting Pass
    const auto& eye = graph.cameraEye();
    float eyeArr[3] = {eye.x, eye.y, eye.z};
    renderGaussianSplatPass(cmd, graph, renderer, stats, graph.viewMatrix().data, camUniforms.proj, eyeArr, width, height);

    if (graph.gizmoProvider_) {
        auto gizmoMeshes = graph.gizmoProvider_(&graph);
        for (auto* gm : gizmoMeshes) {
            if (!gm || gm->mesh().empty()) continue;
            auto& meshBuf = uploadMesh(gm->mesh(), gm);
            MeshDrawCall draw{};
            draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
            draw.indexBuffer = meshBuf.indexBuffer.buffer;
            draw.indexCount = meshBuf.indexCount;
            std::memcpy(draw.modelMatrix, gm->worldMatrix().data, sizeof(draw.modelMatrix));
            std::memcpy(draw.baseColor, gm->color(), sizeof(draw.baseColor));
            std::memcpy(draw.emissiveColor, gm->emissiveColor(), sizeof(draw.emissiveColor));
            draw.emissiveIntensity = gm->emissive();
            draw.metallic = 0.0f;
            draw.roughness = 1.0f;
            draw.flags = 16u;
            passMesh_.drawStatic(cmd, draw);
            hasDrawnMeshes = true;
        }
    }

    hdrTarget_.endRendering(cmd, device_);

    // SSAO Pass
    if (renderer.ssaoEnabled()) {
        passSSAO_.render(cmd, device_, allocator_,
                         depthCopyImage_, camUniforms.proj, camUniforms.invProj,
                         renderer.ssaoRadius(), renderer.ssaoBias());
        passSSAO_.applyAO(cmd, device_, allocator_,
                          hdrTarget_.colorImage().view, width, height,
                          renderer.ssaoIntensity());
    }

    hdrTarget_.transitionColorToShaderRead(cmd, allocator_);

    // Post processing & LUT Pass
    renderPostProcessing(cmd, graph, renderer, width, height);

    // The result stays on the GPU for the presenter; it is copied out for the
    // CPU only when someone read the previous render's pixels.
    if (recordReadbackInline && ensureReadbackBuffer()) {
        recordReadback(cmd);
    } else {
        allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                         ldrPresentationImage_.currentLayout,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ldrPresentationImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    if (device_.submitFrame(cmd) && recordReadbackInline && readbackBuffer_.isValid()) {
        readbackRecordedSerial_ = renderSerial_;
        readbackTicket_ = device_.lastFrameTicket();
    }

    hasMeshContent_ = hasDrawnMeshes;
    renderer.setCullStats(stats);
}

} // namespace bro::scene::vk

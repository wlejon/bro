#include "scene/scene_renderer.h"

#include "scene/atmosphere_irradiance.h"
#include "scene/decal_node.h"
#include "scene/depth_policy.h"
#include "scene/scene_graph.h"
#include "scene/skinned_mesh_node.h"
#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace bro::scene {

using bromath::Vec3;

SceneRenderer::SceneRenderer(SceneGraph& graph) : graph_(graph) {
    // Fallback sun for scenes with no LightNode (see render3D). Configured
    // once; render3D only reads it.
    implicitSun_.setKind(LightNode::Kind::Directional);
    implicitSun_.setDirection(bromath::vnorm(Vec3(-0.3f, -1.0f, -0.5f)));
    implicitSun_.setColor(1.0f, 0.98f, 0.95f);
    implicitSun_.setIntensity(3.0f);
}

SceneRenderer::~SceneRenderer() = default;

int SceneRenderer::targetWidth() const {
    const int w = static_cast<int>(graph_.canvasWidth() * renderScale_ * deviceScale_ + 0.5f);
    return w < 1 ? 1 : w;
}

int SceneRenderer::targetHeight() const {
    const int h = static_cast<int>(graph_.canvasHeight() * renderScale_ * deviceScale_ + 0.5f);
    return h < 1 ? 1 : h;
}

// Conservative world-space bounds per cullable node type. The contract is
// strict — the box must contain everything the node can rasterize this frame,
// so culling can never pop visible content: skinned meshes use palette-posed
// bounds, wind sway pads by its max displacement, splats pad by the 3-sigma
// quad extent the splat shader emits.
std::optional<bromath::AABB3> SceneRenderer::nodeWorldBounds(SceneNode* n) const {
    bromath::AABB3 out;
    switch (n->type()) {
    case SceneNode::Type::Mesh: {
        auto* m = static_cast<MeshNode*>(n);
        if (!m->hasDrawableMesh()) return std::nullopt;
        bromath::AABB3 local = m->localBounds();
        auto* sm = m->asSkinnedMesh();
        if (sm && sm->skinReady()) local = sm->posedLocalBounds();
        out = bromath::atransform(local, m->worldMatrix());
        // Wind sway displaces vertices in world space by at most
        // |windDir| * strength * windMask (per-vertex bend <= 1). cullMargin
        // is the user's promise about custom-vertex-shader displacement —
        // the engine can't infer it from GLSL (see setCullMargin).
        const float pad = m->windMask() * windStrength_ * bromath::vlen(Vec3{windDir_[0], windDir_[1], windDir_[2]}) +
                          m->cullMargin();
        if (pad > 0.0f) {
            out.min = out.min - Vec3{pad, pad, pad};
            out.max = out.max + Vec3{pad, pad, pad};
        }
        return out;
    }
    case SceneNode::Type::InstancedMesh: {
        auto* m = static_cast<InstancedMeshNode*>(n);
        float lo[3], hi[3];
        if (!m->computeWorldInstanceBounds(lo, hi)) return std::nullopt;
        const float pad = m->cullMargin();
        out.min = {lo[0] - pad, lo[1] - pad, lo[2] - pad};
        out.max = {hi[0] + pad, hi[1] + pad, hi[2] + pad};
        return out;
    }
    case SceneNode::Type::GaussianSplat: {
        auto* s = static_cast<GaussianSplatNode*>(n);
        if (s->splatCount() == 0) return std::nullopt;
        // Pad the local centre bounds by the quad extent — kSigma = 3 in the
        // splat VS, plus half a sigma of headroom for the low-pass screen
        // dilation — then take the padded box through the node's world
        // matrix; atransform scales the pad by the node's uniform scale,
        // matching the shader's sigma scaling.
        const float pad = 3.5f * s->maxSigma();
        bromath::AABB3 local = s->localBounds();
        local.min = local.min - Vec3{pad, pad, pad};
        local.max = local.max + Vec3{pad, pad, pad};
        return bromath::atransform(local, s->worldMatrix());
    }
    case SceneNode::Type::Particles3D:
        if (static_cast<Particles3DNode*>(n)->worldBounds(out)) return out;
        return std::nullopt;
    case SceneNode::Type::Decal: {
        // The decal volume is exactly the unit box in local space (node
        // scale IS the size), so the world AABB is that box through the
        // world matrix; the fragment shader discards outside it.
        bromath::AABB3 local;
        local.min = Vec3{-0.5f, -0.5f, -0.5f};
        local.max = Vec3{0.5f, 0.5f, 0.5f};
        return bromath::atransform(local, n->worldMatrix());
    }
    default:
        return std::nullopt;
    }
}

bool SceneRenderer::cameraCulled(SceneNode* n) const {
    if (!cullingActive_) return false;
    auto bounds = nodeWorldBounds(n);
    if (!bounds) return false;
    return !bromath::fintersects(cameraFrustum_, *bounds);
}

void SceneRenderer::collectLights(std::vector<LightNode*>& out) const {
    out.clear();
    const SceneNode* root = graph_.root();
    for (auto& [id, node] : graph_.nodes()) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::Light) continue;
        const SceneNode* p = node.get();
        while (p && p->parent()) p = p->parent();
        if (p != root) continue;
        out.push_back(static_cast<LightNode*>(node.get()));
        if (out.size() >= 32) break;
    }
}

void SceneRenderer::render3D() {
    if (!defaultVulkanContext_) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN("scene: no Vulkan context — the 3D pass is disabled for this "
                     "process (CPU raster path). 2D canvas content still draws.");
        }
        return;
    }
    if (!vkBridge_) {
        vkBridge_ = std::make_unique<vk::SceneVkBridge>(*defaultVulkanContext_);
        if (!vkBridge_->init()) {
            LOG_ERROR("scene: the Vulkan scene renderer failed to initialise; 3D content will not draw");
        }
    }

    cullStats_ = CullStats{};
    cullingActive_ = frustumCullingEnabled_;
    if (cullingActive_)
        cameraFrustum_ = makeFrustum(bromath::mmul(graph_.projectionMatrix(), graph_.viewMatrix()));

    collectLights(frameLights_);
    if (frameLights_.empty()) frameLights_.push_back(&implicitSun_);
    updateSunIrradiance(frameLights_);
    updateSkyAmbient(graph_.cameraEye().y);
    shadowAtlasLimit_ = static_cast<int>(vkBridge_->maxShadowAtlas());
    prepareShadows(frameLights_);
    planShadowTiles();

    hasMeshContent_ = vkBridge_->render3D(graph_, *this, cullStats_);
}

render::LayerImage SceneRenderer::outputImage() const {
    if (!vkBridge_ || !hasMeshContent_) return {};
    return vkBridge_->outputImage();
}

void SceneRenderer::releaseNodes(std::span<const uint32_t> ids) {
    if (vkBridge_) vkBridge_->releaseNodes(ids);
}

std::vector<uint8_t> SceneRenderer::readTonemapPixelsRGBA(int& outW, int& outH) {
    if (vkBridge_) return vkBridge_->readTonemapPixelsRGBA(outW, outH);
    outW = outH = 0;
    return {};
}

bool SceneRenderer::compileCustomShader(CustomShaderTarget target, const std::string& /*key*/,
                                        const std::string& vertexChunk, const std::string& fragmentChunk,
                                        std::string& errOut) {
    return vk::SceneVkCustomShader::validateCustomShader(target, vertexChunk, fragmentChunk, errOut);
}

// --- Colour grading ---------------------------------------------------------

bool SceneRenderer::loadColorLUT(const std::string& path, int size, float amount) {
    broimage::Image img;
    if (!broimage::decode_file(path, img) || img.width <= 0 || img.height <= 0) {
        LOG_ERROR("loadColorLUT: failed to decode '%s'", path.c_str());
        return false;
    }
    const int n = size > 0 ? size : img.height;
    if (n < 2 || img.height != n || img.width != n * n) {
        LOG_ERROR("loadColorLUT: '%s' is %dx%d, expected a %dx%d strip (size^2 x size, size=%d)", path.c_str(),
                  img.width, img.height, n * n, n, n);
        return false;
    }

    const int ch = img.channels;
    std::vector<uint8_t> vox(static_cast<size_t>(n) * n * n * 4, 255);
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            for (int r = 0; r < n; ++r) {
                const size_t src = (static_cast<size_t>(g) * img.width + static_cast<size_t>(b) * n + r) * ch;
                const size_t dst = ((static_cast<size_t>(b) * n + g) * static_cast<size_t>(n) + r) * 4;
                vox[dst + 0] = img.pixels[src + 0];
                vox[dst + 1] = ch > 1 ? img.pixels[src + 1] : img.pixels[src];
                vox[dst + 2] = ch > 2 ? img.pixels[src + 2] : img.pixels[src];
            }
        }
    }
    lutSize_ = n;
    lutAmount_ = amount < 0.0f ? 0.0f : amount;
    lutVoxels_ = std::move(vox);
    lutGeneration_ = nextResourceGeneration();
    return true;
}

void SceneRenderer::clearColorLUT() {
    lutSize_ = 0;
    lutVoxels_.clear();
    lutGeneration_ = nextResourceGeneration();
}

// --- Environment ------------------------------------------------------------

namespace {

// IEEE half from a float, round to nearest even. A finite value past the
// half range clamps to the largest half (65504), as GL's float upload did: an
// HDR sun brighter than that must stay the brightest texel, not become an
// infinity the bake sanitises to black, which darkens every mip it averages
// into and with them the whole diffuse and rough-specular environment
// lighting. Infinity and NaN stay what they are (the bake sanitises both).
uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t absx = x & 0x7FFFFFFFu;
    if (absx >= 0x7F800000u) return static_cast<uint16_t>(sign | 0x7C00u | (absx > 0x7F800000u ? 0x200u : 0u));
    if (absx >= 0x477FF000u) return static_cast<uint16_t>(sign | 0x7BFFu);   // rounds past 65504
    if (absx < 0x38800000u) {                                                 // subnormal or zero
        if (absx < 0x33000000u) return static_cast<uint16_t>(sign);
        const uint32_t mant = (absx & 0x7FFFFFu) | 0x800000u;
        const uint32_t shift = 126u - (absx >> 23);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = ((absx - 0x38000000u) >> 13);
    const uint32_t rem = absx & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<uint16_t>(sign | h);
}

}  // namespace

bool SceneRenderer::loadEnvironment(const std::string& hdrPath) {
    if (hdrPath.empty()) {
        clearEnvironment();
        return true;
    }
    // Top-down float RGBA: row 0 is the +Y pole, which the bake's equirect
    // lookup (v = 0.5 - theta / pi) expects.
    broimage::ImageF32 hdr;
    std::string err;
    if (!broimage::decode_file_f32(hdrPath, hdr, &err) || hdr.width <= 0 || hdr.height <= 0) {
        LOG_ERROR("loadEnvironment: decoding '%s' failed: %s", hdrPath.c_str(), err.c_str());
        return false;
    }
    // The bake runs on the next frame; a panorama the device cannot hold as
    // one image is refused now, so the answer here is the bake's.
    if (defaultVulkanContext_) {
        const uint32_t maxDim = defaultVulkanContext_->deviceProperties().limits.maxImageDimension2D;
        if (static_cast<uint32_t>(hdr.width) > maxDim || static_cast<uint32_t>(hdr.height) > maxDim) {
            LOG_ERROR("loadEnvironment: '%s' is %dx%d, past the device's %u texel limit", hdrPath.c_str(),
                      hdr.width, hdr.height, maxDim);
            return false;
        }
    }
    const size_t texels = static_cast<size_t>(hdr.width) * static_cast<size_t>(hdr.height);
    const int ch = hdr.channels;
    std::vector<uint16_t> half(texels * 4);
    for (size_t i = 0; i < texels; ++i) {
        const float* p = &hdr.pixels[i * ch];
        half[i * 4 + 0] = toHalf(p[0]);
        half[i * 4 + 1] = toHalf(ch > 1 ? p[1] : p[0]);
        half[i * 4 + 2] = toHalf(ch > 2 ? p[2] : p[0]);
        half[i * 4 + 3] = 0x3C00u;   // 1.0
    }
    envPixels_ = std::move(half);
    envWidth_ = hdr.width;
    envHeight_ = hdr.height;
    envPath_ = hdrPath;
    envGeneration_ = nextResourceGeneration();
    return true;
}

void SceneRenderer::clearEnvironment() {
    envPath_.clear();
    envPixels_.clear();
    envWidth_ = envHeight_ = 0;
    envGeneration_ = nextResourceGeneration();
}

void SceneRenderer::updateSunIrradiance(const std::vector<LightNode*>& lights) {
    const LightNode* best = nullptr;
    float bestPower = -1.0f;
    for (const LightNode* l : lights) {
        if (!l || l->kind() != LightNode::Kind::Directional) continue;
        const Vec3& c = l->color();
        const float power = (c.x + c.y + c.z) * (1.0f / 3.0f) * l->intensity();
        if (power > bestPower) {
            bestPower = power;
            best = l;
        }
    }
    if (!best) return;
    const Vec3& c = best->color();
    sunIrradiance_[0] = c.x * best->intensity();
    sunIrradiance_[1] = c.y * best->intensity();
    sunIrradiance_[2] = c.z * best->intensity();
}

void SceneRenderer::updateSkyAmbient(float camY) {
    if (!atmosphere_.enabled) return;
    if (std::abs(camY - skyAmbientCamY_) < 25.0f) return;
    skyAmbientCamY_ = camY;
    AtmosphereParams a = atmosphere_;
    const float* sun = effectiveSunColor();
    a.sunColor[0] = sun[0];
    a.sunColor[1] = sun[1];
    a.sunColor[2] = sun[2];
    computeSkyAmbient(a, camY, skyAmbient_);
}

}  // namespace bro::scene

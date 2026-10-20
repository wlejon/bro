// Far cards: which cells, their captures and their draw (scene_far_cards.h).

#include "scene/vulkan/scene_far_cards.h"

#include "render/vulkan_frames.h"
#include "render/vulkan_util.h"
#include "scene/forest_node.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_forest_cull.h"
#include "scene/vulkan/scene_forest_draw.h"
#include "scene/vulkan/scene_forest_gpu_plan.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_lighting.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"
#include "util/log.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr VkFormat kAtlasFormat = VK_FORMAT_R32G32_UINT;   // far_card_layout.h's texels
constexpr uint32_t kForgetFrames = 600;   // a card not stood in for this long gives its tile back

// far_card_resolve.comp's push block.
struct ResolvePush {
    int32_t src[2];
    int32_t dst[2];
    int32_t size;
    float clearFar;
    float range[2];
};

uint32_t tileSizeFor(float pixels) {
    uint32_t s = FarCards::kMinTile;
    while (s < FarCards::kMaxTile && static_cast<float>(s) < pixels) s <<= 1;
    return s;
}

}  // namespace

bool farCardsEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("BRO_FAR_CARDS");
        return env && *env == '1';
    }();
    return on;
}

// --- Setup ---

bool FarCards::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    if (!farCardsEnabled()) return true;
    VkDevice dev = gpu.device.device();

    // The resolve: the scratch target's colour and depth samples into a tile.
    {
        SceneVkDescriptorLayoutBuilder b;
        b.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        b.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        b.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
        resolveSetLayout_ = b.build(dev);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePush)};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &resolveSetLayout_;
        pi.pushConstantRangeCount = 1;
        pi.pPushConstantRanges = &push;
        if (!resolveSetLayout_ || vkCreatePipelineLayout(dev, &pi, nullptr, &resolveLayout_) != VK_SUCCESS)
            return false;
        VkShaderModule module = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FarCardResolveComp);
        if (!module) return false;
        const VkBool32 reversed = reversedZ() ? VK_TRUE : VK_FALSE;
        const VkSpecializationMapEntry entry{depth::kReversedZConstantId, 0, sizeof(VkBool32)};
        VkSpecializationInfo spec{1, &entry, sizeof(reversed), &reversed};
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.stage.pSpecializationInfo = &spec;
        ci.layout = resolveLayout_;
        const bool ok = vkCreateComputePipelines(dev, gpu.device.context().pipelineCache(), 1, &ci, nullptr,
                                                 &resolvePipeline_) == VK_SUCCESS;
        SceneVkShaderModule::destroy(dev, module);
        if (!ok) {
            resolvePipeline_ = VK_NULL_HANDLE;
            return false;
        }
    }

    // The draw: the camera (set 0), then the records and the atlas.
    {
        SceneVkDescriptorLayoutBuilder b;
        b.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        b.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        drawSetLayout_ = b.build(dev);
        const VkDescriptorSetLayout sets[2] = {gpu.defaults.cameraLayout, drawSetLayout_};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 2;
        pi.pSetLayouts = sets;
        if (!drawSetLayout_ || vkCreatePipelineLayout(dev, &pi, nullptr, &drawLayout_) != VK_SUCCESS) return false;
        vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FarCardVert);
        fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FarCardFrag);
        fsIndirect_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FarCardIndirectFrag);
        if (!vs_ || !fs_ || !fsIndirect_) return false;
    }
    ready_ = true;
    return true;
}

void FarCards::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    drawPipelines_.destroy(gpu.device);
    for (SceneVkImage* img : {&atlas_, &scratchColor_, &scratchDepth_, &clearFar_})
        if (img->isValid()) gpu.allocator.destroyImage(*img);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    SceneVkShaderModule::destroy(dev, fsIndirect_);
    vs_ = fs_ = fsIndirect_ = VK_NULL_HANDLE;
    if (resolvePipeline_) vkDestroyPipeline(dev, resolvePipeline_, nullptr);
    if (resolveLayout_) vkDestroyPipelineLayout(dev, resolveLayout_, nullptr);
    if (drawLayout_) vkDestroyPipelineLayout(dev, drawLayout_, nullptr);
    if (resolveSetLayout_) vkDestroyDescriptorSetLayout(dev, resolveSetLayout_, nullptr);
    if (drawSetLayout_) vkDestroyDescriptorSetLayout(dev, drawSetLayout_, nullptr);
    resolvePipeline_ = VK_NULL_HANDLE;
    resolveLayout_ = drawLayout_ = VK_NULL_HANDLE;
    resolveSetLayout_ = drawSetLayout_ = VK_NULL_HANDLE;
    cards_.clear();
    nodes_.clear();
    pages_ = {};
    captures_.clear();
    records_.clear();
    groups_.clear();
    clearFarReady_ = false;
    ready_ = false;
}

void FarCards::releaseNodes(std::span<const uint32_t> ids) {
    for (uint32_t id : ids) {
        nodes_.erase(id);
        for (auto it = cards_.begin(); it != cards_.end();) {
            if (it->second.node == id) {
                freeTile(it->second.tile);
                it = cards_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool FarCards::ensureImages(SceneGpu& gpu) {
    if (atlas_.isValid()) return true;
    SceneVkAllocator& a = gpu.allocator;
    const VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const bool ok =
        a.createImage(kAtlasSize, kAtlasSize, kAtlasFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      local, atlas_) &&
        a.createImage(kScratch, kScratch, SceneTargets::kHdrFormat,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, local, scratchColor_, 1,
                      VK_SAMPLE_COUNT_4_BIT) &&
        a.createImage(kScratch, kScratch, SceneTargets::kDepthFormat,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, local, scratchDepth_, 1,
                      VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) &&
        a.createImage(kScratch, kScratch, VK_FORMAT_R32_SFLOAT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, local, clearFar_);
    if (!ok) {
        LOG_ERROR("FarCards: failed creating the atlas and capture targets");
        for (SceneVkImage* img : {&atlas_, &scratchColor_, &scratchDepth_, &clearFar_})
            if (img->isValid()) a.destroyImage(*img);
        ready_ = false;
        return false;
    }
    return true;
}

// --- The atlas: pages of one tile size each ---

FarCards::Tile FarCards::allocTile(uint32_t size) {
    const uint32_t perSide = kPage / size;
    const uint32_t slots = perSide * perSide;
    Page* fresh = nullptr;
    uint16_t freshIndex = 0;
    for (uint32_t p = 0; p < kPages; ++p) {
        Page& page = pages_[p];
        if (page.size == size && page.count < slots) {
            for (uint32_t s = 0; s < slots; ++s)
                if (!page.used[s]) {
                    page.used[s] = true;
                    page.count++;
                    return {static_cast<uint16_t>(p), static_cast<uint16_t>(s), static_cast<uint16_t>(size)};
                }
        }
        if (page.size == 0 && !fresh) {
            fresh = &page;
            freshIndex = static_cast<uint16_t>(p);
        }
    }
    if (!fresh) return {};
    fresh->size = size;
    fresh->used.reset();
    fresh->used[0] = true;
    fresh->count = 1;
    return {freshIndex, 0, static_cast<uint16_t>(size)};
}

void FarCards::freeTile(Tile& t) {
    if (!t.valid()) return;
    Page& page = pages_[t.page];
    if (page.used[t.slot]) {
        page.used[t.slot] = false;
        if (--page.count == 0) page.size = 0;
    }
    t = {};
}

uint32_t FarCards::tileX(const Tile& t) const {
    const uint32_t perSide = kPage / t.size;
    return (t.page % (kAtlasSize / kPage)) * kPage + (t.slot % perSide) * t.size;
}

uint32_t FarCards::tileY(const Tile& t) const {
    const uint32_t perSide = kPage / t.size;
    return (t.page / (kAtlasSize / kPage)) * kPage + (t.slot / perSide) * t.size;
}

void FarCards::dropCard(uint64_t key) {
    auto it = cards_.find(key);
    if (it == cards_.end()) return;
    freeTile(it->second.tile);
    auto n = nodes_.find(it->second.node);
    if (n != nodes_.end() && it->second.cell < n->second.carded.size()) n->second.carded[it->second.cell] = 0;
    cards_.erase(it);
}

// --- Selection ---

void FarCards::lightKey(const SceneFrame& frame, float out[16]) const {
    const SceneLightingUniforms& l = frame.lighting;
    const SceneRenderer& r = frame.renderer;
    const float key[16] = {l.sunDirection[0], l.sunDirection[1], l.sunDirection[2], l.sunColor[0],
                           l.sunColor[1],     l.sunColor[2],     l.sunColor[3],     l.ambientColor[0],
                           l.ambientColor[1], l.ambientColor[2], l.atmSunDir[0],    l.atmSunDir[1],
                           l.atmSunDir[2],    r.fogDensity(),    r.fogStart(),      r.fogEnd()};
    std::memcpy(out, key, sizeof(key));
}

void FarCards::putRecord(const Card& c, const bromath::Vec3& eye, float* out) const {
    // Tile clip -> position relative to the eye now.
    const bromath::Mat4 m = bromath::mmul(bromath::mtranslate(c.eye - eye), c.invRelViewProj);
    std::memcpy(out, m.data, 16 * sizeof(float));
    const float inv = 1.0f / static_cast<float>(kAtlasSize);
    const float x = static_cast<float>(tileX(c.tile)), y = static_cast<float>(tileY(c.tile));
    const float s = static_cast<float>(c.tile.size);
    out[16] = x * inv;
    out[17] = y * inv;
    out[18] = (x + s) * inv;
    out[19] = (y + s) * inv;
    out[20] = 0.0f;   // the card's window of samples: all of them (no hand-over band yet)
    out[21] = c.fallbackDepth;
    const uint32_t salt = c.cell * 0x9e3779b9u ^ c.node * 0x85ebca6bu;
    std::memcpy(&out[22], &salt, sizeof(salt));
    out[23] = s;
    out[24] = c.depthRange[0];
    out[25] = c.depthRange[1];
    out[26] = static_cast<float>(c.samples);
    out[27] = 0.0f;
}

void FarCards::select(SceneFrame& frame, const std::vector<ForestNode*>& forests) {
    ++frame_;
    captures_.clear();
    records_.clear();
    groups_.clear();
    frame.forestCellSkip.clear();
    const SceneView& view = frame.view;
    if (!ready_ || !view.perspective || forests.empty() || !forest::cellSkipHonoured()) return;
    if (frame.gpu.targets.samples() != VK_SAMPLE_COUNT_4_BIT && frame.gpu.targets.samples() != VK_SAMPLE_COUNT_1_BIT)
        return;

    const float pxAtMetre = std::fabs(view.proj.at(1, 1)) * static_cast<float>(view.height) * 0.5f;
    const float W = static_cast<float>(view.width), H = static_cast<float>(view.height);
    float light[16];
    lightKey(frame, light);
    const auto lightMoved = [&](const Card& c) {
        for (int i = 0; i < 16; ++i)
            if (std::fabs(c.light[i] - light[i]) > 1e-3f * std::max(1.0f, std::fabs(light[i]))) return true;
        return false;
    };

    struct Want {
        uint64_t key;
        ForestNode* node;
        uint32_t cell;
        float pixels;          // extent on screen
        float across;          // its bounding sphere's diameter on screen
        float cx, cy;          // its rect's centre (px)
        float priority;        // higher first: a stale card's staleness, a new one's value
        bool fresh;            // no card yet
        float value;           // trees a texel
    };
    std::vector<Want> wants;
    struct Standing {
        uint64_t key;
        uint32_t node, cell;
    };
    std::vector<Standing> standing;

    for (ForestNode* node : forests) {
        if (node->instanceCount() == 0 || node->specimenCount() == 0 || node->mesh().empty() || node->effectiveUnlit())
            continue;
        const ForestNode::Lod& lod = node->lod();
        if (!forest::farCellsApply(lod, node->forceTier(), true)) continue;
        const auto& cells = node->cells();
        if (cells.empty()) continue;
        const bromath::Mat4& world = node->worldMatrix();
        NodeCells& nc = nodes_[node->id()];
        if (nc.generation != node->dataGeneration() || std::memcmp(nc.world.data, world.data, sizeof(world.data)) != 0 ||
            nc.bounds.size() != cells.size()) {
            // Its cells' boxes and extents; its cards are of the old trees.
            nc.generation = node->dataGeneration();
            nc.world = world;
            const float nodeScale = std::sqrt(world.data[4] * world.data[4] + world.data[5] * world.data[5] +
                                              world.data[6] * world.data[6]);
            nc.bounds.resize(cells.size());
            nc.extent.assign(cells.size() * 4, 0.0f);
            const auto& order = node->cellOrder();
            const auto& instances = node->instances();
            for (size_t c = 0; c < cells.size(); ++c) {
                nc.bounds[c] = bromath::atransform(cells[c].bounds, world);
                forest::CellExtent e;
                for (uint32_t k = cells[c].first; k < cells[c].first + cells[c].count; ++k)
                    forest::growExtent(e, *node, instances[order[k]], nodeScale);
                nc.extent[c * 4 + 0] = e.clump;
                nc.extent[c * 4 + 1] = e.keep;
                nc.extent[c * 4 + 2] = e.bloom;
                nc.extent[c * 4 + 3] = e.unit;
            }
            std::vector<uint64_t> old;
            for (const auto& [key, card] : cards_)
                if (card.node == node->id()) old.push_back(key);
            for (uint64_t key : old) dropCard(key);
            nc.carded.assign(cells.size(), 0);
        }

        const bromath::Vec3 eye = view.eye;
        // A cell with no card is weighed for one a frame in four (the
        // rectangle on screen is most of the cost, and a cell waiting draws
        // its crowns); a card standing in is checked every frame.
        const uint32_t phase = static_cast<uint32_t>(frame_ & 3);
        for (uint32_t c = 0; c < cells.size(); ++c) {
            const bool carded = nc.carded[c] != 0;
            if (!carded && (c & 3) != phase) continue;
            const bromath::AABB3& b = nc.bounds[c];
            if (!bromath::fintersects(view.frustum, b)) continue;
            const float dx = std::max(std::max(b.min.x - eye.x, eye.x - b.max.x), 0.0f);
            const float dy = std::max(std::max(b.min.y - eye.y, eye.y - b.max.y), 0.0f);
            const float dz = std::max(std::max(b.min.z - eye.z, eye.z - b.max.z), 0.0f);
            const float across = std::sqrt(dx * dx + dz * dz);
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const forest::CellExtent e{nc.extent[c * 4], nc.extent[c * 4 + 1], nc.extent[c * 4 + 2],
                                       nc.extent[c * 4 + 3]};
            if (!forest::crownsAlone(lod, node->leafLength(), pxAtMetre, e, d, across)) continue;
            const uint64_t key = uint64_t{node->id()} << 32 | c;
            const bromath::Vec3 centre = (b.min + b.max) * 0.5f;
            const float radius = 0.5f * bromath::vlen(b.max - b.min);
            const float centreDist = std::max(bromath::vlen(centre - eye), radius + 1.0f);
            const float sphere = 2.0f * radius * pxAtMetre / centreDist;
            float priority = 0.0f;
            if (carded) {
                auto it = cards_.find(key);
                if (it == cards_.end() || !it->second.captured) continue;
                Card& card = it->second;
                // How stale: the eye's turn round the cell, as the relief
                // inside it would shift on screen, against the size.
                const bromath::Vec3 a = bromath::vnorm(centre - card.eye);
                const bromath::Vec3 n = bromath::vnorm(centre - eye);
                const float parallax = bromath::vlen(a - n) * radius * pxAtMetre / std::max(d, 1.0f);
                const float scale = sphere / std::max(card.across, 1.0f);
                float stale = parallax / kParallaxPixels;
                stale = std::max(stale, scale > 1.0f ? (scale - 1.0f) / 0.25f : (1.0f / scale - 1.0f) / 0.25f);
                if (card.generation != node->dataGeneration() || lightMoved(card)) stale = std::max(stale, 4.0f);
                if (stale < 1.0f) {
                    card.used = frame_;
                    standing.push_back({key, node->id(), c});
                    continue;
                }
                priority = stale;
            }
            // Its rectangle on screen (every corner in front of the eye).
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            bool behind = false;
            for (int k = 0; k < 8 && !behind; ++k) {
                const bromath::Vec3 p{(k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y,
                                      (k & 4) ? b.max.z : b.min.z};
                const bromath::Mat4& m = view.viewProj;
                const float cx = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
                const float cy = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
                const float cw = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
                if (cw <= view.nearZ) {
                    behind = true;
                    break;
                }
                const float sx = (cx / cw * 0.5f + 0.5f) * W, sy = (cy / cw * 0.5f + 0.5f) * H;
                x0 = std::min(x0, sx);
                x1 = std::max(x1, sx);
                y0 = std::min(y0, sy);
                y1 = std::max(y1, sy);
            }
            // (A texel past each side for the rasterizer's reach.)
            const float pixels = std::max(x1 - x0, y1 - y0) + 2.0f;
            if (behind || pixels > static_cast<float>(kMaxTile)) {
                // Grown past a card (or round behind the eye): its crowns again.
                if (carded) dropCard(key);
                continue;
            }
            Want want{key, node, c, pixels, sphere, 0.5f * (x0 + x1), 0.5f * (y0 + y1), priority, !carded, 0.0f};
            if (carded) {
                // A stale card stands in until its recapture.
                Card& card = cards_[key];
                card.used = frame_;
                standing.push_back({key, node->id(), c});
                want.value = card.value;
            } else {
                // What a card would save for the room it takes.
                const float side = static_cast<float>(tileSizeFor(pixels));
                want.value = static_cast<float>(cells[c].count) / (side * side);
                want.priority = want.value;
            }
            wants.push_back(want);
        }
    }

    // This frame's captures: the stalest cards first (they stand in, showing
    // their error), then new cards, the most trees a texel first; as many as
    // fit the scratch target (shelves, the largest first).
    std::stable_sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) {
        if (a.fresh != b.fresh) return !a.fresh;
        return a.priority > b.priority;
    });
    if (wants.size() > kMaxCaptures) wants.resize(kMaxCaptures);
    std::stable_sort(wants.begin(), wants.end(),
                     [](const Want& a, const Want& b) { return tileSizeFor(a.pixels) > tileSizeFor(b.pixels); });
    uint32_t shelfX = 0, shelfY = 0, shelfH = 0;
    if (!wants.empty() && !ensureImages(frame.gpu)) return;
    for (const Want& w : wants) {
        const uint32_t size = tileSizeFor(w.pixels);
        if (shelfX + size > kScratch) {
            shelfX = 0;
            shelfY += shelfH;
            shelfH = 0;
        }
        if (shelfY + size > kScratch) break;
        Tile* held = nullptr;
        if (auto it = cards_.find(w.key); it != cards_.end()) held = &it->second.tile;
        Tile tile;
        if (held && held->valid() && held->size == size) {
            tile = *held;
        } else {
            if (held) freeTile(*held);
            tile = allocTile(size);
            // A full atlas gives back the card longest unused, else the one
            // standing in that saves the least for its room, if it saves
            // less than this one would.
            while (!tile.valid()) {
                uint64_t victim = 0;
                bool found = false, victimStanding = true;
                uint64_t victimUsed = ~uint64_t{0};
                float victimValue = w.value;
                for (const auto& [k, c] : cards_) {
                    if (k == w.key || !c.tile.valid() || c.capturedAt == frame_) continue;
                    const bool isStanding = c.used == frame_;
                    if (!isStanding) {
                        if (victimStanding || c.used < victimUsed) {
                            victim = k;
                            victimUsed = c.used;
                            victimStanding = false;
                            found = true;
                        }
                    } else if (victimStanding && c.value < victimValue) {
                        victim = k;
                        victimValue = c.value;
                        found = true;
                    }
                }
                if (!found) break;
                dropCard(victim);
                tile = allocTile(size);
            }
            if (!tile.valid()) {
                // No room: a stale card gives way to its crowns; the new
                // cards after this one save less still.
                if (held) dropCard(w.key);
                if (w.fresh) break;
                continue;
            }
        }
        Card& card = cards_[w.key];
        card.node = w.node->id();
        card.cell = w.cell;
        card.tile = tile;
        card.value = w.value;
        nodes_[card.node].carded[w.cell] = 1;
        // The capture: the camera, its projection cropped to a square of
        // `size` pixels round the cell's rectangle, at one texel a pixel,
        // centred on a pixel corner (size is even) so its texels are the
        // screen's pixels and the card samples them unblurred until the eye
        // moves.
        const float sx = W / static_cast<float>(size), sy = H / static_cast<float>(size);
        const float ncx = 2.0f * std::round(w.cx) / W - 1.0f, ncy = 2.0f * std::round(w.cy) / H - 1.0f;
        bromath::Mat4 crop = bromath::midentity();
        crop.at(0, 0) = sx;
        crop.at(0, 3) = -sx * ncx;
        crop.at(1, 1) = sy;
        crop.at(1, 3) = -sy * ncy;
        const bromath::Mat4 proj = bromath::mmul(crop, view.proj);
        // (make() takes bromath's clip convention: toVulkanClip is its own inverse.)
        Capture cap;
        cap.key = w.key;
        cap.node = w.node;
        cap.cell = w.cell;
        cap.view = SceneView::make(view.view, toVulkanClip(proj), view.eye, view.nearZ, view.farZ, true, size, size);
        cap.x = shelfX;
        cap.y = shelfY;
        captures_.push_back(cap);
        shelfX += size;
        shelfH = std::max(shelfH, size);

        card.generation = w.node->dataGeneration();
        card.eye = view.eye;
        card.invRelViewProj = bromath::mmul(cap.view.invRelView, cap.view.invProj);
        card.pixels = w.pixels;
        std::memcpy(card.light, light, sizeof(light));
        const NodeCells& nc = nodes_[card.node];
        const bromath::Vec3 centre = (nc.bounds[w.cell].min + nc.bounds[w.cell].max) * 0.5f - view.eye;
        const bromath::Mat4& m = cap.view.relViewProj;
        const float cz = m.at(2, 0) * centre.x + m.at(2, 1) * centre.y + m.at(2, 2) * centre.z + m.at(2, 3);
        const float cw = m.at(3, 0) * centre.x + m.at(3, 1) * centre.y + m.at(3, 2) * centre.z + m.at(3, 3);
        card.fallbackDepth = cw > 0.0f ? cz / cw : depth::clearFar();
        // The depths the 24 bits span: the cell's box seen from the capture
        // (every corner is in front of the eye), a little wider.
        float zLo = 1e30f, zHi = -1e30f;
        for (int k = 0; k < 8; ++k) {
            const bromath::AABB3& b = nc.bounds[w.cell];
            const bromath::Vec3 p = bromath::Vec3{(k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y,
                                                  (k & 4) ? b.max.z : b.min.z} -
                                    view.eye;
            const float z = m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3);
            const float zw = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
            const float zd = z / std::max(zw, 1e-6f);
            zLo = std::min(zLo, zd);
            zHi = std::max(zHi, zd);
        }
        const float pad = std::max((zHi - zLo) * 0.01f, 1e-7f);
        card.depthRange[0] = std::clamp(zLo - pad, 0.0f, 1.0f);
        card.depthRange[1] = std::clamp(zHi + pad, 0.0f, 1.0f);
        if (card.depthRange[1] <= card.depthRange[0]) card.depthRange[1] = card.depthRange[0] + 1e-7f;
        card.samples = kCaptureSamples;
        card.capturedAt = frame_;
        card.across = w.across;
        if (w.fresh) standing.push_back({w.key, card.node, w.cell});   // (a stale card already stands)
        card.captured = true;
        card.used = frame_;
    }

    // The cards standing in this frame: their records, and their cells
    // left out of the camera's plan.
    // Grouped by tile size (a draw a grid size).
    std::vector<std::pair<uint32_t, const Card*>> shown;
    shown.reserve(standing.size());
    for (const Standing& s : standing) {
        auto it = cards_.find(s.key);
        if (it == cards_.end() || !it->second.captured || !it->second.tile.valid()) continue;
        std::vector<uint32_t>& skip = frame.forestCellSkip[s.node];
        if (skip.empty()) skip.assign(nodes_[s.node].bounds.size(), 0u);
        skip[s.cell] = 1u;
        shown.emplace_back(it->second.tile.size, &it->second);
    }
    std::sort(shown.begin(), shown.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    records_.resize(shown.size() * FAR_CARD_RECORD_VEC4S * 4);
    groups_.clear();
    for (size_t i = 0; i < shown.size(); ++i) {
        putRecord(*shown[i].second, view.eye, records_.data() + i * FAR_CARD_RECORD_VEC4S * 4);
        const uint32_t grid = std::max(1u, shown[i].first / FAR_CARD_GRID_TEXELS);
        const uint32_t vertices = grid * grid * 6;
        if (groups_.empty() || groups_.back().vertices != vertices)
            groups_.push_back({vertices, static_cast<uint32_t>(i), 0});
        groups_.back().count++;
    }

    // Cards long unused give their tiles back.
    if ((frame_ & 63) == 0) {
        std::vector<uint64_t> forget;
        for (const auto& [key, card] : cards_)
            if (card.used + kForgetFrames < frame_) forget.push_back(key);
        for (uint64_t key : forget) dropCard(key);
    }
}

// --- Capture ---

void FarCards::capture(SceneFrame& frame) {
    if (captures_.empty() || !ensureImages(frame.gpu)) return;
    SceneGpu& gpu = frame.gpu;
    VkCommandBuffer cmd = frame.cmd;
    VkDevice dev = gpu.device.device();

    // The far depth a capture's sprites test against: the far plane.
    if (!clearFarReady_) {
        render::cmdTransitionImage(cmd, clearFar_.image, render::colorRange(), VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue far{};
        far.float32[0] = depth::clearFar();
        const VkImageSubresourceRange range = render::colorRange();
        vkCmdClearColorImage(cmd, clearFar_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &range);
        render::cmdTransitionImage(cmd, clearFar_.image, render::colorRange(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        clearFar_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        clearFarReady_ = true;
    }

    // The scratch target, whole, cleared.
    const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    render::cmdTransitionImage(cmd, scratchColor_.image, render::colorRange(), VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    render::cmdTransitionImage(cmd, scratchDepth_.image, depthRange, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = scratchColor_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = scratchDepth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {depth::clearFar(), 0};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {kScratch, kScratch}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    info.pDepthAttachment = &depthAtt;
    gpu.device.cmdBeginRendering(cmd, &info);

    TargetFormat target = TargetFormat::colorOnly(SceneTargets::kHdrFormat, VK_SAMPLE_COUNT_4_BIT);
    target.depth = SceneTargets::kDepthFormat;
    // One lighting set for them all: the frame's (the same eye), its far
    // depth the far plane.
    const VkDescriptorSet lightingSet = writeLightingSet(gpu, frame.lighting, VK_NULL_HANDLE, nullptr, clearFar_.view);
    SceneGpuResources& res = gpu.resources;
    for (const Capture& cap : captures_) {
        ForestNode& node = *cap.node;
        const CellContent& cell = node.cells()[cap.cell];
        if (cell.count == 0) continue;
        const std::vector<float>& data = node.gpuData();
        const SceneVkBuffer* store =
            res.storage(node.id(), node.dataGeneration(), data.data(), data.size() * sizeof(float));
        if (!store) continue;
        // Its trees' crowns, each the entry a crown alone takes (addTree's).
        constexpr size_t kEntryFloats = FOREST_ENTRY_RECORDS * 4;
        const VkDeviceSize bytes = VkDeviceSize{cell.count} * kEntryFloats * sizeof(float);
        const render::UploadSlice slice = gpu.device.frameUpload(bytes);
        if (!slice) continue;
        auto* dst = static_cast<float*>(slice.mapped);
        const auto& order = node.cellOrder();
        for (uint32_t j = 0; j < cell.count; ++j) {
            float* d = dst + j * kEntryFloats;
            std::memset(d, 0, kEntryFloats * sizeof(float));
            d[FOREST_E_INSTANCE] = std::bit_cast<float>(order[cell.first + j]);
            d[FOREST_E_FIRST] = std::bit_cast<float>(0u);
            d[FOREST_E_END] = std::bit_cast<float>(1u);
            d[FOREST_E_PARAM] = std::bit_cast<float>(0u);
            d[FOREST_E_WIDENING] = 1.0f;
            d[FOREST_E_KEEP_END] = 0.0f;
            d[FOREST_E_TAIL] = 1.0f;
            d[FOREST_E_FADE] = 1.0f;
            d[FOREST_E_SLOT] = std::bit_cast<float>(~0u);
        }
        VkDescriptorSet set = gpu.device.frameSet(gpu.defaults.boneLayout);
        if (set == VK_NULL_HANDLE) continue;
        SceneVkDescriptorWriter writer;
        writer.writeBuffer(1, store->buffer, store->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.writeBuffer(2, slice.buffer, bytes, slice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // (No leaf is posed: any buffer.)
        writer.writeBuffer(3, store->buffer, store->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.updateSet(dev, set);
        forest::DrawBase base;
        if (!forest::drawBase(frame, gpu.defaults, dev, node, set, base)) continue;
        base.lod = node.lod();
        base.shadowLod = node.lod();
        MeshDraw draw = forest::groupDraw(frame, node, base, forest::Crowns, false, 0, false, 0, cell.count);
        // Raw radiance: alpha is coverage, not the SSR mask.
        const auto flags = static_cast<uint32_t>(draw.push.pbrParams[3]);
        draw.push.pbrParams[3] = static_cast<float>(flags & ~mesh_flags::kReflectance);
        draw.lightingSet = VK_NULL_HANDLE;

        const SceneCameraUniforms cam = cap.view.uniforms(&frame.renderer);
        const VkDescriptorBufferInfo camInfo = gpu.device.frameUniform(&cam, sizeof(cam));
        VkDescriptorSet cameraSet = gpu.device.frameSet(gpu.defaults.cameraLayout);
        SceneVkDescriptorWriter camWriter;
        camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
        camWriter.updateSet(dev, cameraSet);

        const uint32_t size = cap.view.width;
        const VkViewport viewport{static_cast<float>(cap.x), static_cast<float>(cap.y), static_cast<float>(size),
                                  static_cast<float>(size), 0.0f, 1.0f};
        const VkRect2D scissor{{static_cast<int32_t>(cap.x), static_cast<int32_t>(cap.y)}, {size, size}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        gpu.meshes.record(cmd, target, cameraSet, lightingSet, draw);
    }
    gpu.device.cmdEndRendering(cmd);

    // Resolve each into its tile.
    render::cmdTransitionImage(cmd, scratchColor_.image, render::colorRange(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    render::cmdTransitionImage(cmd, scratchDepth_.image, depthRange, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        // (Waiting for the last frames' reads of the tiles rewritten.)
        render::ImageBarrier b;
        b.image = atlas_.image;
        b.oldLayout = atlas_.currentLayout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        b.srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        b.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
        render::cmdImageBarrier(cmd, b);
        atlas_.currentLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkDescriptorSet set = gpu.device.frameSet(resolveSetLayout_);
    SceneVkDescriptorWriter w;
    w.writeImage(0, scratchColor_.view, gpu.defaults.nearestSampler);
    w.writeImage(1, scratchDepth_.view, gpu.defaults.nearestSampler);
    w.writeImage(2, atlas_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.updateSet(dev, set);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolveLayout_, 0, 1, &set, 0, nullptr);
    for (const Capture& cap : captures_) {
        auto it = cards_.find(cap.key);
        if (it == cards_.end() || !it->second.tile.valid()) continue;
        const Card& card = it->second;
        ResolvePush p{};
        p.src[0] = static_cast<int32_t>(cap.x);
        p.src[1] = static_cast<int32_t>(cap.y);
        p.dst[0] = static_cast<int32_t>(tileX(card.tile));
        p.dst[1] = static_cast<int32_t>(tileY(card.tile));
        p.size = card.tile.size;
        p.clearFar = depth::clearFar();
        p.range[0] = card.depthRange[0];
        p.range[1] = card.depthRange[1];
        vkCmdPushConstants(cmd, resolveLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const uint32_t groups = (card.tile.size + 7) / 8;
        vkCmdDispatch(cmd, groups, groups, 1);
    }
    {
        render::ImageBarrier b;
        b.image = atlas_.image;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        b.srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        b.dstAccess = VK_ACCESS_SHADER_READ_BIT;
        render::cmdImageBarrier(cmd, b);
        atlas_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

// --- Draw ---

void FarCards::draw(SceneFrame& frame) {
    if (records_.empty() || !atlas_.isValid() || atlas_.currentLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    SceneGpu& gpu = frame.gpu;
    VkCommandBuffer cmd = frame.cmd;
    const VkDeviceSize bytes = records_.size() * sizeof(float);
    const render::UploadSlice slice = gpu.device.frameUpload(bytes);
    if (!slice) return;
    std::memcpy(slice.mapped, records_.data(), bytes);
    VkDescriptorSet set = gpu.device.frameSet(drawSetLayout_);
    if (set == VK_NULL_HANDLE) return;
    SceneVkDescriptorWriter w;
    w.writeBuffer(0, slice.buffer, bytes, slice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    w.writeImage(1, atlas_.view, gpu.defaults.nearestSampler);
    w.updateSet(gpu.device.device(), set);

    const bool indirect = frame.hdrTarget.colorCount > 1;
    VkPipeline pipeline = drawPipelines_.get(indirect ? 1 : 0, frame.hdrTarget, [&] {
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, indirect ? fsIndirect_ : fs_)
            .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setPolygonMode(VK_POLYGON_MODE_FILL)
            .setCullMode(VK_CULL_MODE_NONE)
            .setTarget(frame.hdrTarget)
            .disableBlending(indirect ? 2 : 1)
            .enableDepthTest(true)
            .setSpecialization(1, frame.hdrTarget.samples == VK_SAMPLE_COUNT_4_BIT);
        return b.build(gpu.device.device(), drawLayout_);
    });
    if (!pipeline) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet sets[2] = {frame.cameraSet, set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout_, 0, 2, sets, 0, nullptr);
    for (const DrawGroup& g : groups_) vkCmdDraw(cmd, g.vertices, g.count, 0, g.first);
    frame.drewContent = true;
}

}  // namespace bro::scene::vk

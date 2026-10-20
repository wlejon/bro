#include "scene/vulkan/scene_forest_gpu_plan.h"

#include "render/vulkan_pipeline_cache.h"
#include "render/vulkan_queue.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_timer.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"
#include "scene/vulkan/shaders/world_cells_layout.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bro::scene::vk::forest {

namespace {

uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
float floatOf(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
uint32_t roundUp(uint32_t n, uint32_t to) { return (n + to - 1) / to * to; }

// The forest's cell bands, by a cell's bounding radius in pixels on the
// camera's screen: a 64 m cell (a radius near 48 m) is near within about
// 110 m and far past about 1.8 km at 1364 px high and 60 degrees. Every band is still planned tree by tree; the far field (step 4 of
// docs/scene-world-architecture.md) will take the far band's cells.
constexpr float kNearCellPixels = 512.0f;
constexpr float kFarCellPixels = 32.0f;

// The bark classes' workgroups a node (forest_plan_measure.comp's
// PLAN_CLASSIFY, each walking every this many workgroups' worth of the
// segments): about the device's SMs twice, so the trees beside the eye,
// thousands of segments each, spread over the GPU.
constexpr uint32_t kClassifyGroups = 128;

// The plan walks the trees of the cells in view (the world cell pass's);
// BRO_WORLD_CELLS=0 walks every tree, for A/B (the plan is the same).
bool worldCellsOn() {
    static const bool on = [] {
        const char* env = std::getenv("BRO_WORLD_CELLS");
        return !(env && std::strcmp(env, "0") == 0);
    }();
    return on;
}

constexpr BuiltinSceneShader kStageShaders[] = {
    BuiltinSceneShader::ForestPlanMeasureComp,    BuiltinSceneShader::ForestPlanClassifyComp,
    BuiltinSceneShader::ForestPlanScanLocalComp,
    BuiltinSceneShader::ForestPlanScanAddComp,    BuiltinSceneShader::ForestPlanSortHistComp,
    BuiltinSceneShader::ForestPlanSortScatterComp, BuiltinSceneShader::ForestPlanLayoutComp,
    BuiltinSceneShader::ForestPlanItemsComp,      BuiltinSceneShader::ForestPlanCostComp,
    BuiltinSceneShader::ForestPlanVaryComp,       BuiltinSceneShader::ForestPlanSettleComp,
    BuiltinSceneShader::ForestPlanCountComp,      BuiltinSceneShader::ForestPlanArgsComp,
    BuiltinSceneShader::ForestPlanWriteComp,
};
constexpr const char* kStageNames[] = {"forest plan measure", "forest plan classify", "forest plan scan", "forest plan scan add",
                                       "forest plan sort hist", "forest plan sort scatter", "forest plan layout",
                                       "forest plan items", "forest plan cost", "forest plan vary",
                                       "forest plan settle", "forest plan count", "forest plan args",
                                       "forest plan write"};

VkPipeline computePipeline(SceneVkDevice& device, VkShaderModule module, VkPipelineLayout layout, const char* name) {
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    ci.flags = render::pipelineStatsFlags();
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device.device(), device.context().pipelineCache(), 1, &ci, nullptr, &p) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    render::setPipelineStatsLabel(name);
    render::logPipelineStats(device.device(), p);
    return p;
}

// The bisection's asks (FP_G_TABLE): fitDetail's float sqrt(lo * hi) at
// every node of its three possible trees, heap-numbered from 1.
void fillTable(uint32_t* table, int node, float lo, float hi, int depth) {
    if (depth == 6) return;
    const float mid = std::sqrt(lo * hi);
    table[node] = bitsOf(mid);
    fillTable(table, 2 * node, lo, mid, depth + 1);
    fillTable(table, 2 * node + 1, mid, hi, depth + 1);
}

}  // namespace

// --- A node's read-backs ---

void GpuPlanner::Node::poll() {
    if (!readback.mappedData) return;
    // At most a render late: the render before the newest is waited for.
    // With two frames in flight it is done (or all but) by the time the
    // next is prepared, so this costs a frame nothing; unwaited, a script's
    // read two renders after a cut saw the view before it (headless, where
    // the newest render is still on the GPU when the script runs).
    int newest = -1, before = -1;
    for (int i = 0; i < kRing; ++i) {
        const Slot& s = ring_[i];
        if (s.state != Slot::Submitted) continue;
        if (newest < 0 || s.serial > ring_[newest].serial) {
            before = newest;
            newest = i;
        } else if (before < 0 || s.serial > ring_[before].serial) {
            before = i;
        }
    }
    if (before >= 0 && ring_[before].serial > readSerial_) ring_[before].queue->wait(ring_[before].ticket);
    int best = -1;
    for (int i = 0; i < kRing; ++i) {
        const Slot& s = ring_[i];
        if (s.state != Slot::Submitted || s.serial <= readSerial_ || !s.queue->isComplete(s.ticket)) continue;
        if (best < 0 || s.serial > ring_[best].serial) best = i;
    }
    if (best < 0) return;
    const auto* w = static_cast<const uint32_t*>(readback.mappedData) + best * FP_S_WORDS;
    const auto u64 = [&](int at) { return uint64_t{w[at]} | uint64_t{w[at + 1]} << 32; };
    ForestNode::Stats s;
    for (int k = 0; k < ForestNode::kTiers; ++k) s.trees[k] = w[FP_S_TREES + k];
    s.culled = w[FP_S_CULLED];
    s.leaves = u64(FP_S_LEAVES);
    s.blooms = u64(FP_S_BLOOMS);
    s.vertices = u64(FP_S_VERTICES);
    s.shadowVertices = u64(FP_S_SHADOW_VERTICES);
    s.entries = w[FP_S_ENTRIES];
    s.draws = w[FP_S_DRAWS];
    s.detail = floatOf(w[FP_S_DETAIL]);
    s.shadowDetail = floatOf(w[FP_S_SHADOW_DETAIL]);
    newest_ = s;
    askedEntries_ = s.entries;
    askedPoses_ = w[FP_S_POSES];    have_ = true;
    readSerial_ = ring_[best].serial;
    for (Slot& slot : ring_)
        if (slot.state == Slot::Submitted && slot.serial <= readSerial_ && slot.queue->isComplete(slot.ticket))
            slot.state = Slot::Free;
}

bool GpuPlanner::Node::latest(ForestNode::Stats& out) {
    poll();
    if (!have_) return false;
    out = newest_;
    return true;
}

// --- Setup ---

bool GpuPlanner::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    limits_ = gpu.device.context().deviceProperties().limits;
    // The planner's draws each start at their own first entry, an instance
    // index from the indirect command: without the feature, the CPU plans.
    if (!gpu.device.context().features().drawIndirectFirstInstance) {
        LOG_INFO("forest planner: drawIndirectFirstInstance unsupported; forests plan on the CPU");
        return true;
    }
    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t b = 0; b < 7; ++b) builder.addBinding(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = builder.build(device_);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout_;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &push;
    if (!setLayout_ || vkCreatePipelineLayout(device_, &pi, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("forest planner: failed creating its layout");
        return false;
    }
    for (int s = 0; s < kStages; ++s) {
        modules_[s] = SceneVkShaderCompiler::createBuiltinModule(device_, kStageShaders[s]);
        pipelines_[s] = modules_[s] ? computePipeline(gpu.device, modules_[s], layout_, kStageNames[s]) : VK_NULL_HANDLE;
        if (!pipelines_[s]) {
            LOG_ERROR("forest planner: failed creating the %s stage", kStageNames[s]);
            return false;
        }
    }
    // The leaf poses of its draws: the camera (set 0), its set (1: the
    // dispatches and first entries), the draw's set 3, the draw's push.
    const std::array<VkDescriptorSetLayout, 4> poseSets = {gpu.defaults.cameraLayout, setLayout_,
                                                           gpu.defaults.emptyLayout, gpu.defaults.boneLayout};
    VkPushConstantRange posePush{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MeshPushConstants)};
    pi.setLayoutCount = static_cast<uint32_t>(poseSets.size());
    pi.pSetLayouts = poseSets.data();
    pi.pPushConstantRanges = &posePush;
    poseModule_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::ForestLeafPoseIndirectComp);
    if (!poseModule_ || vkCreatePipelineLayout(device_, &pi, nullptr, &poseLayout_) != VK_SUCCESS ||
        !(posePipeline_ = computePipeline(gpu.device, poseModule_, poseLayout_, "forest leaf pose indirect"))) {
        LOG_ERROR("forest planner: failed creating its pose stage");
        return false;
    }
    // (Without the cell pass every tree is walked, as BRO_WORLD_CELLS=0.)
    if (worldCellsOn() && !cellsPass_.setup(gpu)) {
        LOG_ERROR("forest planner: no world cell pass; the plan walks every tree");
        cellsPass_.cleanup(gpu);
    }
    // (Shared: uploaded on the graphics queue, then the plan's alone.)
    if (!gpu.allocator.createBuffer(FP_G_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shared_, /*shared=*/true))
        return false;
    ready_ = true;
    return true;
}

void GpuPlanner::destroyNode(SceneVkAllocator& allocator, Node& n) {
    for (SceneVkBuffer* b : {&n.staticBuf, &n.work, &n.readback})
        if (b->isValid()) allocator.destroyBuffer(*b);
    for (uint32_t s = 0; s < Node::kSlots; ++s) {
        for (SceneVkBuffer* b : {&n.slotEntries_[s], &n.slotArgs_[s]})
            if (b->isValid()) allocator.destroyBuffer(*b);
        n.slotEntryCap_[s] = 0;
    }
    n.entries = n.args = {};
    n.entryCap = 0;
    WorldCellsPass::destroy(allocator, n.cellTarget_);
}

void GpuPlanner::cleanup(SceneGpu& gpu) {
    for (auto& [key, n] : nodes_) destroyNode(gpu.allocator, *n);
    nodes_.clear();
    queued_.clear();
    poseJobs_.clear();
    if (shared_.isValid()) gpu.allocator.destroyBuffer(shared_);
    sharedReady_ = false;
    cellsPass_.cleanup(gpu);
    for (int s = 0; s < kStages; ++s) {
        if (pipelines_[s]) vkDestroyPipeline(device_, pipelines_[s], nullptr);
        SceneVkShaderModule::destroy(device_, modules_[s]);
        pipelines_[s] = VK_NULL_HANDLE;
        modules_[s] = VK_NULL_HANDLE;
    }
    if (posePipeline_) vkDestroyPipeline(device_, posePipeline_, nullptr);
    SceneVkShaderModule::destroy(device_, poseModule_);
    if (poseLayout_) vkDestroyPipelineLayout(device_, poseLayout_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    posePipeline_ = VK_NULL_HANDLE;
    poseModule_ = VK_NULL_HANDLE;
    poseLayout_ = layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ready_ = false;
}

std::shared_ptr<GpuPlanner::Node> GpuPlanner::node(const ForestNode& n) {
    auto& slot = nodes_[&n];
    if (!slot) slot = std::make_shared<Node>();
    slot->used = ++use_;
    return slot;
}

bool GpuPlanner::ensureBuffer(SceneFrame& frame, SceneVkBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags props, VkDeviceSize headroom, bool shared) {
    if (buf.isValid() && buf.size >= size) return true;
    // (The old one stays until the frames that read it are done.)
    if (buf.isValid()) frame.gpu.allocator.destroyBuffer(buf);
    if (!frame.gpu.allocator.createBuffer(size + headroom, usage, props, buf, shared)) {
        buf = {};
        return false;
    }
    return true;
}

// --- A node's frame ---

bool GpuPlanner::buildStatic(SceneFrame& frame, Node& n, const ForestNode& node,
                             const std::vector<bromath::AABB3>& bounds, float reach) {
    const auto& specimens = node.specimens();
    const auto& instances = node.instances();
    // The specimens' words, each frame: a look's change moves them (their
    // wood's shade, a leaflet's run) without a new data generation.
    std::vector<uint32_t> spec(specimens.size() * FP_SPEC_WORDS, 0u);
    for (size_t k = 0; k < specimens.size(); ++k) {
        const ForestNode::Specimen& sp = specimens[k];
        uint32_t* w = spec.data() + k * FP_SPEC_WORDS;
        w[FP_SPEC_SEG_START] = sp.segStart;
        w[FP_SPEC_SEG_COUNT] = sp.segCount;
        w[FP_SPEC_CLUMP_START] = sp.clumpStart;
        w[FP_SPEC_CLUMP_COUNT] = sp.clumpCount;
        w[FP_SPEC_LEAF_START] = sp.leafStart;
        w[FP_SPEC_LEAF_COUNT] = sp.leafCount;
        w[FP_SPEC_BLOOM_START] = sp.bloomStart;
        w[FP_SPEC_BLOOM_COUNT] = sp.bloomCount;
        w[FP_SPEC_CROWN] = bitsOf(sp.crown.x);
        w[FP_SPEC_CROWN + 1] = bitsOf(sp.crown.y);
        w[FP_SPEC_CROWN + 2] = bitsOf(sp.crown.z);
        w[FP_SPEC_CROWN_RADIUS] = bitsOf(sp.crownRadius);
        w[FP_SPEC_CROWN_UP] = bitsOf(sp.crownRadiusUp);
        w[FP_SPEC_CLUMP_RADIUS] = bitsOf(sp.meanClumpRadius);
        w[FP_SPEC_TRUNK_RADIUS] = bitsOf(sp.trunkRadius);
        w[FP_SPEC_BLOOM_EXTENT] = bitsOf(sp.bloomSize * ForestNode::bloomExtent(sp.bloomForm));
        w[FP_SPEC_PINNATE] = node.pinnate(k) ? 1u : 0u;
        w[FP_SPEC_WOOD_SEEN] = bitsOf(node.woodSeen(k));
        float over[3];
        node.barkOverLeaf(k, over);
        for (int i = 0; i < 3; ++i) w[FP_SPEC_BARK_OVER_LEAF + i] = bitsOf(over[i]);
    }
    const bromath::Mat4& world = node.worldMatrix();
    const bool same = n.staticBuf.isValid() && n.generation == node.dataGeneration() && n.reach == reach &&
                      n.trees == instances.size() && std::memcmp(n.world, world.data, sizeof(n.world)) == 0 &&
                      spec == n.specimenWords;
    if (same) return true;

    const auto& keep = node.keepRadius();
    const auto& area = node.woodArea();
    const auto& clumps = node.clumpRecords();
    const auto& twigs = node.clumpTwigs();
    const auto& segments = node.segmentRecords();
    const uint32_t N = static_cast<uint32_t>(instances.size());
    const uint32_t clumpCount = clumps.size() >= 4 ? static_cast<uint32_t>((clumps.size() - 4) / 8 + 1) : 0u;
    const uint32_t segmentCount = static_cast<uint32_t>(segments.size() / 8);
    uint32_t head[FP_ST_HEADER] = {};
    uint32_t at = FP_ST_HEADER;
    head[FP_ST_TREES] = at;
    at += N * FP_TREE_WORDS;
    head[FP_ST_SPECIMENS] = at;
    at += static_cast<uint32_t>(spec.size());
    head[FP_ST_KEEP] = at;
    at += static_cast<uint32_t>(keep.size());
    head[FP_ST_AREA] = at;
    at += static_cast<uint32_t>(area.size());
    head[FP_ST_CLUMPS] = at;
    head[FP_ST_CLUMP_COUNT] = clumpCount;
    at += clumpCount * 4;
    head[FP_ST_TWIGS] = at;
    head[FP_ST_TWIG_COUNT] = static_cast<uint32_t>(twigs.size());
    at += static_cast<uint32_t>(twigs.size()) * 2;
    head[FP_ST_SEGMENTS] = at;
    head[FP_ST_SEGMENT_COUNT] = segmentCount;
    at += segmentCount * 8;
    const auto& order = node.cellOrder();
    if (order.size() != N) return false;
    head[FP_ST_ORDER] = at;
    at += N;
    std::vector<uint32_t> words(at, 0u);
    std::memcpy(words.data() + head[FP_ST_ORDER], order.data(), size_t{N} * 4);
    std::memcpy(words.data(), head, sizeof(head));
    for (uint32_t i = 0; i < N; ++i) {
        const ForestNode::Instance& in = instances[i];
        uint32_t* w = words.data() + head[FP_ST_TREES] + i * FP_TREE_WORDS;
        const bromath::AABB3& b = bounds[i];
        const float v[] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z,
                           in.position.x, in.position.y, in.position.z, in.scale, in.cosYaw, in.sinYaw};
        for (int j = 0; j < 12; ++j) w[j] = bitsOf(v[j]);
        w[FP_TREE_SPECIMEN] = in.specimen;
    }
    std::memcpy(words.data() + head[FP_ST_SPECIMENS], spec.data(), spec.size() * 4);
    for (size_t i = 0; i < keep.size(); ++i) words[head[FP_ST_KEEP] + i] = bitsOf(keep[i]);
    for (size_t i = 0; i < area.size(); ++i) words[head[FP_ST_AREA] + i] = bitsOf(area[i]);
    for (uint32_t c = 0; c < clumpCount; ++c)
        for (int j = 0; j < 4; ++j) words[head[FP_ST_CLUMPS] + c * 4 + j] = bitsOf(clumps[c * 8 + j]);
    for (size_t c = 0; c < twigs.size(); ++c) {
        words[head[FP_ST_TWIGS] + c * 2] = twigs[c].stems;
        words[head[FP_ST_TWIGS] + c * 2 + 1] = twigs[c].pieces;
    }
    for (uint32_t s = 0; s < segmentCount * 8; ++s) words[head[FP_ST_SEGMENTS] + s] = bitsOf(segments[s]);

    const VkDeviceSize bytes = VkDeviceSize{words.size()} * 4;
    if (bytes > limits_.maxStorageBufferRange) return false;
    // A new buffer each time, never rewritten in place: the last frame's plan
    // may still read the old one on the compute queue, which the upload on
    // the graphics queue is not ordered against. The plan waits for the
    // upload (computeReadsGraphics).
    if (n.staticBuf.isValid()) frame.gpu.allocator.destroyBuffer(n.staticBuf);
    if (!ensureBuffer(frame, n.staticBuf, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, /*shared=*/true) ||
        !frame.gpu.allocator.stageAndUploadBuffer(n.staticBuf.buffer, words.data(), bytes)) {
        if (n.staticBuf.isValid()) frame.gpu.allocator.destroyBuffer(n.staticBuf);
        return false;
    }
    frame.gpu.device.computeReadsGraphics();
    // Its cells, each bounded by its trees' boxes as the plan tests them (so
    // a tree a view sees is in a cell it sees, to the last bit).
    std::vector<CellContent> cells = node.cells();
    for (CellContent& c : cells) {
        c.bounds = bromath::aempty3();
        for (uint32_t k = c.first; k < c.first + c.count; ++k) {
            c.bounds.min = bromath::vmin(c.bounds.min, bounds[order[k]].min);
            c.bounds.max = bromath::vmax(c.bounds.max, bounds[order[k]].max);
        }
    }
    n.cellsGeneration_ = nextResourceGeneration();
    n.cells_.setContent(n.provider_, std::move(cells), n.cellsGeneration_);
    n.generation = node.dataGeneration();
    n.reach = reach;
    n.trees = instances.size();
    std::memcpy(n.world, world.data, sizeof(n.world));
    n.specimenWords.swap(spec);
    return true;
}

bool GpuPlanner::begin(SceneFrame& frame, Node& n, const ForestNode& node, VkBuffer store,
                       const std::vector<bromath::AABB3>& bounds, float reach, uint32_t tiles,
                       std::vector<uint32_t>& params) {
    if (!ready_) return false;
    // Nodes gone (or long unplanned) give their buffers back.
    if ((use_ & 1023) == 0)
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (it->second->used + 4096 < use_) {
                destroyNode(frame.gpu.allocator, *it->second);
                it = nodes_.erase(it);
            } else {
                ++it;
            }
        }
    if (!sharedReady_) {
        // Zeros (no detail settled yet) and the bisection's asks.
        std::vector<uint32_t> init(FP_G_WORDS, 0u);
        const float trees[3][2] = {{1.0f, 2.0f}, {2.0f, 4.0f}, {4.0f, 8.0f}};
        for (int t = 0; t < 3; ++t) fillTable(init.data() + FP_G_TABLE + t * 64, 1, trees[t][0], trees[t][1], 0);
        if (!frame.gpu.allocator.stageAndUploadBuffer(shared_.buffer, init.data(), init.size() * 4)) return false;
        frame.gpu.device.computeReadsGraphics();
        sharedReady_ = true;
    }
    // A leaf store new since the last plan (its upload on the graphics queue).
    if (store != n.store_) {
        n.store_ = store;
        frame.gpu.device.computeReadsGraphics();
    }
    n.poll();
    // A frame recorded but never submitted reads back nothing.
    for (Node::Slot& s : n.ring_)
        if (s.state == Node::Slot::Recorded) s.state = Node::Slot::Free;
    n.slotThisFrame_ = -1;
    if (bounds.size() != node.instances().size() || !buildStatic(frame, n, node, bounds, reach)) return false;

    // The work buffer's runs for this many trees and tiles.
    const uint32_t N = static_cast<uint32_t>(node.instances().size());
    if (N == 0 || N >= (1u << FP_VIEW_SHIFT) || tiles > FP_MAX_TILES) return false;
    const uint32_t stride = roundUp(N, FP_GROUP_SIZE);
    n.tiles = tiles;
    n.aligned = roundUp(N, FP_SCAN_BLOCK);
    n.items = (1 + tiles) * stride;
    n.itemGroups = n.items / FP_GROUP_SIZE;
    const uint32_t blocks = n.aligned / FP_SCAN_BLOCK;
    const uint32_t flags = tiles * N + 1, sums = FP_COLUMNS * n.itemGroups + 1;
    const uint32_t widest = std::max({flags, sums, FP_SORT_DIGITS * blocks});
    const uint32_t scratch = widest / (FP_SCAN_BLOCK - 1) + 16;
    const uint32_t sizes[FP_W_RUNS] = {N * FP_CAM_WORDS, flags,    n.items,   n.aligned, n.aligned, n.aligned,
                                       n.aligned,        FP_SORT_DIGITS * blocks, n.items, sums,      scratch,
                                       n.items,          n.aligned, N * FP_CLASSIFY_WORDS};
    uint64_t at = FP_N_WORDS;
    for (int r = 0; r < FP_W_RUNS; ++r) {
        n.runs[r] = static_cast<uint32_t>(at);
        at += sizes[r];
    }
    const uint32_t maxGroups = limits_.maxComputeWorkGroupCount[0];
    if (at * 4 > limits_.maxStorageBufferRange || n.aligned / FP_GROUP_SIZE > maxGroups ||
        n.itemGroups > maxGroups || (flags + FP_SCAN_BLOCK - 1) / FP_SCAN_BLOCK > maxGroups)
        return false;
    constexpr VkBufferUsageFlags kWork =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!ensureBuffer(frame, n.work, at * 4, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, at))
        return false;

    // The entries: room for what the last frames asked and half again
    // (at first twice the trees: most of a large forest is a crown each).
    // This frame slot's, as are the args: the last frame's draws may still
    // read theirs while this plan writes these.
    const uint32_t f = frame.gpu.device.frames().frameIndex();
    SceneVkBuffer& entries = n.slotEntries_[f];
    uint32_t& entryCap = n.slotEntryCap_[f];
    const VkDeviceSize entryBytes = VkDeviceSize{FOREST_ENTRY_RECORDS} * 16;
    const uint64_t maxCap = limits_.maxStorageBufferRange / entryBytes;
    const uint64_t asked = n.askedEntries_;
    const uint64_t need = std::min<uint64_t>(std::max<uint64_t>({asked, 65536u, uint64_t{N} * 2}), maxCap);
    if (!entries.isValid() || entryCap < need) {
        const uint64_t cap = std::min<uint64_t>(std::max(need, asked + asked / 2), maxCap);
        if (entries.isValid()) frame.gpu.allocator.destroyBuffer(entries);
        if (!frame.gpu.allocator.createBuffer(cap * entryBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, entries, /*shared=*/true)) {
            entries = {};
            entryCap = 0;
            return false;
        }
        entryCap = static_cast<uint32_t>(cap);
    }
    if (!ensureBuffer(frame, n.slotArgs_[f], FP_A_WORDS * 4,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, /*shared=*/true))
        return false;
    n.entries = entries;
    n.entryCap = entryCap;
    n.args = n.slotArgs_[f];
    if (!n.readback.isValid()) {
        if (!frame.gpu.allocator.createBuffer(Node::kRing * FP_S_WORDS * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                              n.readback) ||
            !n.readback.mappedData) {
            if (n.readback.isValid()) frame.gpu.allocator.destroyBuffer(n.readback);
            return false;
        }
        for (Node::Slot& s : n.ring_) s = {};
    }

    // Its cells on the GPU, when the plan walks them.
    n.cellsOn = worldCellsOn() && cellsPass_.ready() &&
                cellsPass_.update(frame, n.cellTarget_, n.cells_, n.provider_, n.cellsGeneration_);

    params.resize(FP_P_WORDS, 0u);
    params[FP_P_TREES] = N;
    params[FP_P_TILES] = tiles;
    params[FP_P_ENTRY_CAP] = n.entryCap;
    params[FP_P_ITEMS] = n.items;
    params[FP_P_ALIGNED] = n.aligned;
    params[FP_P_ITEM_GROUPS] = n.itemGroups;
    for (int r = 0; r < FP_W_RUNS; ++r) params[FP_P_WORK + r] = n.runs[r];
    return true;
}

bool GpuPlanner::add(SceneFrame& frame, Node& n, const std::vector<uint32_t>& params) {
    const VkDeviceSize bytes = FP_P_WORDS * 4;
    const VkDeviceSize align = std::max<VkDeviceSize>(limits_.minStorageBufferOffsetAlignment, 16);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes, align);
    if (!slice || params.size() < FP_P_WORDS) return false;
    std::memcpy(slice.mapped, params.data(), bytes);
    n.set = frame.gpu.device.frameSet(setLayout_);
    if (n.set == VK_NULL_HANDLE) return false;
    SceneVkDescriptorWriter writer;
    constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writer.writeBuffer(0, n.staticBuf.buffer, n.staticBuf.size, 0, kStorage);
    writer.writeBuffer(1, slice.buffer, bytes, slice.offset, kStorage);
    writer.writeBuffer(2, n.work.buffer, n.work.size, 0, kStorage);
    writer.writeBuffer(3, shared_.buffer, shared_.size, 0, kStorage);
    writer.writeBuffer(4, n.entries.buffer, n.entries.size, 0, kStorage);
    writer.writeBuffer(5, n.args.buffer, n.args.size, 0, kStorage);
    // The cell pass's views, the plan's own: the camera, then each tile.
    if (n.cellsOn) {
        std::vector<uint32_t> planes(size_t{1 + n.tiles} * 24);
        std::memcpy(planes.data(), params.data() + FP_P_FRUSTUM, 24 * 4);
        for (uint32_t t = 0; t < n.tiles; ++t)
            std::memcpy(planes.data() + (1 + t) * 24, params.data() + FP_P_TILE + t * FP_TILE_WORDS + FP_TILE_FRUSTUM,
                        24 * 4);
        // Each view's pixel scale: the camera's focal length, a tile's texel.
        std::vector<WorldCellsPass::Scale> scales(1 + n.tiles);
        scales[0].focalPixels = floatOf(params[FP_P_PX_AT_METRE]);
        for (uint32_t t = 0; t < n.tiles; ++t) {
            const uint32_t at = FP_P_TILE + t * FP_TILE_WORDS;
            scales[1 + t].texel = floatOf(params[at + FP_TILE_TEXEL_CONST]);
            scales[1 + t].texelPerMetre = params[at + FP_TILE_ORTHO] ? 0.0f : floatOf(params[at + FP_TILE_TEXEL_PER_METRE]);
        }
        WorldCellsPass::Views views;
        for (int j = 0; j < 3; ++j) views.eye[j] = floatOf(params[FP_P_EYE + j]);
        views.count = 1 + n.tiles;
        views.planes = planes.data();
        views.scales = scales.data();
        views.nearPixels = kNearCellPixels;
        views.farPixels = kFarCellPixels;
        views.itemGroup = FP_GROUP_SIZE;
        n.cellsOn = cellsPass_.begin(frame, n.cellTarget_, views, n.cells_, n.provider_);
    }
    // (The measure stage's cells; any buffer when it walks every tree.)
    const SceneVkBuffer& cells = n.cellsOn ? n.cellTarget_.out : n.staticBuf;
    writer.writeBuffer(6, cells.buffer, cells.size, 0, kStorage);
    writer.updateSet(device_, n.set);
    queued_.push_back(&n);
    return true;
}

void GpuPlanner::addPoseJob(Node& n, const MeshPushConstants& push, uint32_t group, VkDescriptorSet boneSet) {
    PoseJob job;
    job.node = &n;
    job.push = push;
    job.push.extra[2] = static_cast<float>(group);
    job.group = group;
    job.boneSet = boneSet;
    poseJobs_.push_back(job);
}

// --- Recording ---

void GpuPlanner::barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &b, 0, nullptr, 0, nullptr);
}

void GpuPlanner::dispatch(VkCommandBuffer cmd, Stage s, const Node& n, uint32_t groups, const Push& p) const {
    if (groups == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[s]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &n.set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cmd, groups, 1, 1);
}

void GpuPlanner::dispatchItems(VkCommandBuffer cmd, Stage s, const Node& n, const Push& p) const {
    dispatchIndirect(cmd, s, n, FP_A_ITEM_DISPATCH, p);
}

void GpuPlanner::dispatchIndirect(VkCommandBuffer cmd, Stage s, const Node& n, uint32_t argsWord,
                                  const Push& p) const {
    dispatchIndirect(cmd, s, n, n.args.buffer, VkDeviceSize{argsWord} * 4, p);
}

void GpuPlanner::dispatchIndirect(VkCommandBuffer cmd, Stage s, const Node& n, VkBuffer buffer, VkDeviceSize offset,
                                  const Push& p) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[s]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &n.set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatchIndirect(cmd, buffer, offset);
}

// An exclusive scan in place of each queued node's WORK run `run`, of a
// length the GPU set (planScanSized's words from `words`, dispatches from
// ARGS word `args`), at most `most[i]` long for the i-th node: each block's
// scan, the blocks' sums scanned the same way a level up (each level's at
// the next's offset in the scratch run), then added back down. A level the
// count leaves one block scans to a single sum, which the levels above it
// scan to zero and add nothing. The nodes go a level at a time, a barrier
// a level for them all.
void GpuPlanner::scanSized(VkCommandBuffer cmd, uint32_t run, uint32_t words, uint32_t args,
                           const std::vector<uint32_t>& most) const {
    struct Levels {
        uint32_t at[FP_SCAN_LEVELS + 1];
        uint32_t count = 0;
    };
    std::vector<Levels> nodes(queued_.size());
    uint32_t deepest = 0;
    for (size_t i = 0; i < queued_.size(); ++i) {
        Levels& L = nodes[i];
        L.at[0] = queued_[i]->runs[run];
        uint32_t scratch = queued_[i]->runs[FP_W_SCRATCH];
        for (uint32_t count = most[i]; count > FP_SCAN_BLOCK && L.count + 1 < FP_SCAN_LEVELS; ++L.count) {
            const uint32_t blocks = (count + FP_SCAN_BLOCK - 1) / FP_SCAN_BLOCK;
            L.at[L.count + 1] = scratch;
            scratch += blocks;
            count = blocks;
        }
        deepest = std::max(deepest, L.count);
    }
    const auto push = [&](const Levels& L, uint32_t l, uint32_t sums) {
        return Push{0, L.at[l], FP_SCAN_COUNT_WORD | (words + l), sums};
    };
    for (uint32_t l = 0; l < deepest; ++l) {
        for (size_t i = 0; i < queued_.size(); ++i)
            if (l < nodes[i].count) dispatchIndirect(cmd, ScanLocal, *queued_[i], args + 4 * l, push(nodes[i], l, nodes[i].at[l + 1]));
        barrier(cmd);
    }
    for (size_t i = 0; i < queued_.size(); ++i)
        dispatch(cmd, ScanLocal, *queued_[i], 1, push(nodes[i], nodes[i].count, 0xffffffffu));
    for (uint32_t l = deepest; l-- > 0;) {
        barrier(cmd);
        for (size_t i = 0; i < queued_.size(); ++i)
            if (l < nodes[i].count) dispatchIndirect(cmd, ScanAdd, *queued_[i], args + 4 * l, push(nodes[i], l, nodes[i].at[l + 1]));
    }
}

void GpuPlanner::record(SceneFrame& frame) {
    if (queued_.empty() && poseJobs_.empty()) return;
    VkCommandBuffer cmd = frame.computeStream();
    // On the async compute queue the draws are on another queue: the frame's
    // join (a timeline wait) orders them after the plan, and its barriers
    // name only the stages a compute queue has.
    const bool apart = frame.computeApart();
    // (The timed passes' stages, BRO_SCENE_GPU_PASSES.)
    const auto mark = [&](const char* name) {
        if (frame.timer) frame.timer->mark(cmd, name);
    };
    if (!queued_.empty()) {
        // Last frame's draws, poses and read-backs used what this rewrites.
        VkMemoryBarrier start{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        start.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT;
        start.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &start, 0,
                             nullptr, 0, nullptr);
        vkCmdFillBuffer(cmd, shared_.buffer, 0, FP_G_FRAME_WORDS * 4, 0u);
        for (Node* n : queued_) vkCmdFillBuffer(cmd, n->work.buffer, 0, FP_N_WORDS * 4, 0u);
        barrier(cmd);
        // The cells in view, of the nodes that walk them.
        std::vector<WorldCellsPass::Target*> cellTargets;
        for (Node* n : queued_)
            if (n->cellsOn) cellTargets.push_back(&n->cellTarget_);
        if (!cellTargets.empty()) {
            cellsPass_.record(cmd, cellTargets);
            barrier(cmd);
            mark("plan:cells");
        }
        // Each tree's measure (of the cells in view, or all), the camera's
        // order, each view's items.
        for (Node* n : queued_) {
            if (n->cellsOn)
                dispatchIndirect(cmd, Measure, *n, n->cellTarget_.out.buffer, VkDeviceSize{WC_O_DISPATCH} * 4,
                                 {1, 0, 0, 0});
            else
                dispatch(cmd, Measure, *n, (static_cast<uint32_t>(n->trees) + FP_GROUP_SIZE - 1) / FP_GROUP_SIZE);
        }
        barrier(cmd);
        mark("plan:measure");
        for (uint32_t pass = 0; pass < FP_SORT_PASSES; ++pass) {
            // (The bark classes beside the sort's first pass, which does
            // not read them.)
            if (pass == 0)
                for (Node* n : queued_) dispatch(cmd, Classify, *n, kClassifyGroups);
            for (Node* n : queued_) dispatchIndirect(cmd, SortHist, *n, FP_A_SORT_DISPATCH, {pass, 0, 0, 0});
            barrier(cmd);
            for (Node* n : queued_) dispatchIndirect(cmd, SortScatter, *n, FP_A_SORT_DISPATCH, {pass, 0, 0, 0});
            barrier(cmd);
        }
        mark("plan:sort");
        std::vector<uint32_t> most(queued_.size());
        for (size_t i = 0; i < queued_.size(); ++i)
            most[i] = queued_[i]->tiles * static_cast<uint32_t>(queued_[i]->trees) + 1;
        scanSized(cmd, FP_W_FLAGS, FP_N_SCAN_FLAGS, FP_A_SCAN_FLAGS, most);
        barrier(cmd);
        for (Node* n : queued_) dispatch(cmd, Layout, *n, 1);
        barrier(cmd);
        for (Node* n : queued_) dispatchIndirect(cmd, Items, *n, FP_A_FILL_DISPATCH, Push{});
        barrier(cmd);
        mark("plan:items");
        // The budget's search, every node's items at each round's ask, then
        // its fits settled.
        // (Rounds 0 and 1 over every item; round 1 lists those whose cost
        // varies, and the later rounds walk the list alone.)
        for (uint32_t round = 0; round < FP_ROUNDS; ++round) {
            for (Node* n : queued_) {
                if (round < 2)
                    dispatchItems(cmd, Cost, *n, {round, 0, 0, 0});
                else
                    dispatchIndirect(cmd, Cost, *n, FP_A_VARY_DISPATCH, {round, 1, 0, 0});
            }
            barrier(cmd);
            if (round == 1) {
                for (Node* n : queued_) dispatch(cmd, Vary, *n, 1);
                barrier(cmd);
            }
        }
        dispatch(cmd, Settle, *queued_.front(), 1);
        barrier(cmd);
        mark("plan:cost");
        // The entries: counted, placed, written.
        for (Node* n : queued_) dispatchItems(cmd, Count, *n);
        barrier(cmd);
        for (size_t i = 0; i < queued_.size(); ++i) most[i] = FP_COLUMNS * queued_[i]->itemGroups + 1;
        scanSized(cmd, FP_W_SUMS, FP_N_SCAN_SUMS, FP_A_SCAN_SUMS, most);
        barrier(cmd);
        for (Node* n : queued_) {
            // (In turn: each takes its pose slots after the last's.)
            dispatch(cmd, Args, *n, 1);
            barrier(cmd);
        }
        mark("plan:count");
        for (Node* n : queued_) dispatchItems(cmd, Write, *n);
        VkMemoryBarrier done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                             VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             (apart ? 0 : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &done, 0, nullptr, 0, nullptr);
        mark("plan:write");
        // The stats, to the read-back ring.
        for (Node* n : queued_) {
            const int slot = n->cursor_;
            n->cursor_ = (n->cursor_ + 1) % Node::kRing;
            n->ring_[slot].state = Node::Slot::Recorded;
            n->ring_[slot].serial = ++n->serial_;
            n->ring_[slot].ticket = 0;
            n->slotThisFrame_ = slot;
            VkBufferCopy copy{VkDeviceSize{FP_N_STATS} * 4, VkDeviceSize(slot) * FP_S_WORDS * 4,
                              VkDeviceSize{FP_S_WORDS} * 4};
            vkCmdCopyBuffer(cmd, n->work.buffer, n->readback.buffer, 1, &copy);
        }
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                             0, nullptr);
    }
    if (!poseJobs_.empty()) {
        // The posed groups' leaves, an entry a workgroup, as many as ARGS
        // left them (none for a group drawn inline).
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, posePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 0, 1, &frame.cameraSet, 0, nullptr);
        for (const PoseJob& job : poseJobs_) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 1, 1, &job.node->set, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 3, 1, &job.boneSet, 0, nullptr);
            vkCmdPushConstants(cmd, poseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MeshPushConstants), &job.push);
            vkCmdDispatchIndirect(cmd, job.node->args.buffer, VkDeviceSize{FP_A_POSE + 4 * job.group} * 4);
        }
        if (!apart) {
            VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1,
                                 &after, 0, nullptr, 0, nullptr);
        }
    }
    queued_.clear();
    poseJobs_.clear();
}

void GpuPlanner::submitted(uint64_t ticket, render::VulkanQueue& queue) {
    for (auto& [key, n] : nodes_)
        for (Node::Slot& s : n->ring_)
            if (s.state == Node::Slot::Recorded) {
                s.state = Node::Slot::Submitted;
                s.ticket = ticket;
                s.queue = &queue;
            }
}

}  // namespace bro::scene::vk::forest

#include "scene/vulkan/ground/scene_land_regions.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_queue.h"
#include "render/vulkan_util.h"
#include "scene/nodes/land_node.h"
#include "scene/nodes/world_module.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/shaders/ground/land_process_layout.h"

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr VkFormat kFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkDeviceSize kTexelBytes = 16;
constexpr VkDeviceSize kStorageAlign = 256;
// A level is wanted from kLead times the height its cell is a pixel at, so
// it is in before its detail shows.
constexpr double kLead = 4.0;

void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                         0, nullptr, 0, nullptr);
}

}  // namespace

bool SceneLandRegions::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    SceneVkDescriptorLayoutBuilder set;
    for (uint32_t b = 0; b <= 3; ++b) set.addBinding(b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = set.build(device_);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, LR_P_WORDS * 4};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    const uint32_t zeros[16] = {};
    if (!setLayout_ || vkCreatePipelineLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS ||
        !gpu.allocator.createStorageImage(1, 1, 1, kFormat, none_, false, 1) ||
        !gpu.allocator.createBuffer(sizeof(zeros), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, noneTable_) ||
        !gpu.allocator.stageAndUploadBuffer(noneTable_.buffer, zeros, sizeof(zeros))) {
        LOG_ERROR("SceneLandRegions: failed creating the regions' layout");
        return false;
    }
    return true;
}

void SceneLandRegions::cleanup(SceneGpu& gpu) {
    for (auto& [id, land] : lands_) release(gpu, *land);
    lands_.clear();
    for (AsyncComputePipeline& p : pipelines_) p.destroy(device_);
    pipelines_.clear();
    for (auto& s : sets_) s->destroy(device_);
    sets_.clear();
    byHash_.clear();
    if (none_.isValid()) gpu.allocator.destroyImage(none_);
    if (noneTable_.isValid()) gpu.allocator.destroyBuffer(noneTable_);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void SceneLandRegions::release(SceneGpu& gpu, Land& land) {
    if (land.slots.isValid()) gpu.allocator.destroyImage(land.slots);
    for (auto& pair : land.run)
        for (SceneVkImage& g : pair)
            if (g.isValid()) gpu.allocator.destroyImage(g);
    if (land.data.isValid()) gpu.allocator.destroyBuffer(land.data);
    if (land.tableBuffer.isValid()) gpu.allocator.destroyBuffer(land.tableBuffer);
    for (SceneVkBuffer& b : land.readbacks)
        if (b.isValid()) gpu.allocator.destroyBuffer(b);
}

void SceneLandRegions::forget(SceneGpu& gpu, uint32_t id) {
    const auto it = lands_.find(id);
    if (it == lands_.end()) return;
    release(gpu, *it->second);
    lands_.erase(it);
}

VkImageView SceneLandRegions::slots(uint32_t id) const {
    const auto it = lands_.find(id);
    return it != lands_.end() && it->second->slots.isValid() ? it->second->slots.view : none_.view;
}

VkDescriptorBufferInfo SceneLandRegions::table(uint32_t id) const {
    const auto it = lands_.find(id);
    if (it != lands_.end() && it->second->table.buffer != VK_NULL_HANDLE) return it->second->table;
    return {noneTable_.buffer, 0, noneTable_.size};
}

void SceneLandRegions::counts(uint32_t id, uint32_t& in, uint32_t& pending) const {
    in = pending = 0;
    const auto it = lands_.find(id);
    if (it == lands_.end()) return;
    for (const auto& [k, r] : it->second->resident) in += r.in ? 1u : 0u;
    pending = it->second->pending;
}

VkPipeline SceneLandRegions::pipeline(const LandNode& node, bool* pending) {
    const std::string dispatch = node.regionDispatch();
    const std::string grid = node.gridConfig();
    const std::string regions = node.regionConfig();
    const LandProcess& p = node.process();
    const uint64_t key = scene::moduleSourceHash(p.source) ^ (scene::moduleSourceHash(dispatch) * 0x9e3779b97f4a7c15ull) ^
                         (scene::moduleSourceHash(grid) * 0xc2b2ae3d27d4eb4full) ^
                         (scene::moduleSourceHash(regions) * 0x165667b19e3779f9ull);
    auto it = byHash_.find(key);
    if (it == byHash_.end()) {
        // Compiled off the frame thread (scene_shader_warmup.h); the regions
        // start once it is in.
        auto set = std::make_unique<LandRegionShaders>();
        set->start(p.source, dispatch, grid, regions);
        set->index = static_cast<uint32_t>(sets_.size());
        sets_.push_back(std::move(set));
        pipelines_.emplace_back();
        it = byHash_.emplace(key, static_cast<int>(sets_.size() - 1)).first;
    }
    const size_t i = static_cast<size_t>(it->second);
    LandRegionShaders& set = *sets_[i];
    std::string err;
    const bool wasPending = set.state() == ModuleShaders::State::Pending;
    const ModuleShaders::State st = set.poll(device_, err);
    if (pending) *pending = st == ModuleShaders::State::Pending;
    if (st == ModuleShaders::State::Failed && wasPending)
        LOG_ERROR("Land node %u: its regions' kernels did not compile, so it has no regions:\n%s", node.id(),
                  err.c_str());
    if (st != ModuleShaders::State::Ready) return VK_NULL_HANDLE;
    // (Its pipeline built off the frame thread: pending until it is in.)
    if (!pipelines_[i].started()) pipelines_[i].start(device_, set[LandRegionShaders::Run], layout_, "land region");
    const VkPipeline built = pipelines_[i].get();
    if (pending) *pending = !built && pipelines_[i].usable();
    return built;
}

bool SceneLandRegions::start(SceneGpu& gpu, const LandNode& node, Land& land) {
    const LandRegionLattice& L = node.regionLattice();
    const LandProcess& p = node.process();
    land.pipeline = pipeline(node);
    if (!land.pipeline) return false;
    const uint32_t cells = static_cast<uint32_t>(L.config.cells);
    const int slots = L.slots();
    if (!gpu.allocator.createStorageImage(cells, cells, 1, kFormat, land.slots, false,
                                          static_cast<uint32_t>(slots * L.config.keep)))
        return false;
    for (auto& pair : land.run)
        for (SceneVkImage& r : pair)
            if (!gpu.allocator.createStorageImage(cells, cells, 1, kFormat, r, false,
                                                  static_cast<uint32_t>(p.channels)))
                return false;
    std::vector<float> data(LandProcess::kMaxData * 4, 0.0f);
    std::copy_n(p.data.begin(), std::min(p.data.size(), data.size()), data.begin());
    if (!gpu.allocator.createBuffer(data.size() * sizeof(float),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, land.data) ||
        !gpu.allocator.stageAndUploadBuffer(land.data.buffer, data.data(), data.size() * sizeof(float)))
        return false;
    for (SceneVkBuffer& b : land.readbacks)
        if (!gpu.allocator.createBuffer(static_cast<VkDeviceSize>(cells) * cells * kTexelBytes,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, gpu.allocator.readbackMemory(), b) ||
            !b.mappedData)
            return false;
    if (!gpu.allocator.createBuffer(static_cast<VkDeviceSize>(L.tableSize() + 2 * LandRegionConfig::kMaxLevels) * 16,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, land.tableBuffer))
        return false;
    land.tableDirty = true;
    land.freeSlots.clear();
    for (int s = slots - 1; s >= 0; --s) land.freeSlots.push_back(s);
    LOG_INFO("Land node %u: %d regional slots of %u x %u (%.0f MB)", node.id(), slots, cells, cells,
             static_cast<double>(slots * L.config.keep + 2 * kBatch * p.channels + kReadbacks) * cells * cells *
                 kTexelBytes / 1048576.0);
    return true;
}

void SceneLandRegions::receive(SceneFrame& frame, LandNode& node, Land& land) {
    const int cells = node.regionLattice().config.cells;
    for (auto it = land.reading.begin(); it != land.reading.end();) {
        if (!it->ticketed || !frame.gpu.device.context().queue().isComplete(it->ticket)) {
            ++it;
            continue;
        }
        auto region = std::make_shared<LandRegion>();
        region->key = it->key;
        const auto* texels = static_cast<const float*>(land.readbacks[it->buffer].mappedData);
        region->texels.assign(texels, texels + static_cast<size_t>(cells) * cells * 4);
        land.readbackBusy[it->buffer] = false;
        const auto r = land.resident.find(it->key.packed());
        if (r != land.resident.end()) {
            r->second.in = true;
            land.tableDirty = true;
            node.regionStore().add(std::move(region));
            node.regionChanged(it->key, land.centres[it->key.level]);
        }
        it = land.reading.erase(it);
    }
}

void SceneLandRegions::drop(LandNode& node, Land& land, uint32_t packed) {
    const auto it = land.resident.find(packed);
    if (it == land.resident.end()) return;
    const Resident r = it->second;
    land.resident.erase(it);
    land.freeSlots.push_back(r.slot);
    if (r.in) {
        land.tableDirty = true;
        node.regionStore().remove(r.key);
        node.regionChanged(r.key, land.centres[r.key.level]);
    }
    // The finer regions made from it go with it.
    std::vector<uint32_t> finer;
    for (const auto& [k, o] : land.resident)
        if (o.key.level < r.key.level && o.in && landRegionsOverlap(node.regionLattice(), r.key, o.key))
            finer.push_back(k);
    for (uint32_t k : finer) drop(node, land, k);
}

bool SceneLandRegions::begin(LandNode& node, Land& land, const std::vector<LandRegionKey>& wanted,
                             const std::vector<LandRegionKey>& near) {
    const LandRegionLattice& L = node.regionLattice();
    // A region may start once every wanted region coarser than it that it
    // reads is in: those it overlaps (grown by two of their cells, the
    // read's reach), and on another root any (a face's lattice runs past its
    // edges onto the next).
    std::vector<LandRegionKey> missing;
    for (const LandRegionKey& k : wanted) {
        const auto r = land.resident.find(k.packed());
        if (r == land.resident.end() || !r->second.in) missing.push_back(k);
    }
    // (Coarser region c is read by finer region k.)
    const auto reads = [&](const LandRegionKey& k, const LandRegionKey& c) {
        if (c.root != k.root) return true;
        const double sk = L.config.cells * L.config.cellOf(k.level);
        const double kx = L.originX(k), kz = L.originZ(k);
        const double m = 2.0 * L.config.cellOf(c.level);
        const double sc = L.config.cells * L.config.cellOf(c.level);
        const double cx = L.originX(c), cz = L.originZ(c);
        return cx - m < kx + sk && kx < cx + sc + m && cz - m < kz + sk && kz < cz + sc + m;
    };
    const auto startable = [&](const LandRegionKey& k) {
        if (land.resident.count(k.packed())) return false;
        for (const LandRegionKey& c : missing)
            if (c.level > k.level && reads(k, c)) return false;
        return true;
    };
    // The regions the ground under the eye waits on: those holding it, and
    // the coarser ones they read; first, in `wanted`'s order (coarser
    // first, nearer first), then the rest.
    std::vector<LandRegionKey> order;
    std::vector<LandRegionKey> nearMissing;
    for (const LandRegionKey& k : near)
        if (std::find(missing.begin(), missing.end(), k) != missing.end()) nearMissing.push_back(k);
    std::vector<bool> urgent(wanted.size(), false);
    for (size_t i = 0; i < wanted.size(); ++i) {
        const LandRegionKey& k = wanted[i];
        for (const LandRegionKey& n : nearMissing)
            if (k == n || (k.level > n.level && reads(n, k))) urgent[i] = true;
    }
    for (size_t i = 0; i < wanted.size(); ++i)
        if (urgent[i]) order.push_back(wanted[i]);
    for (size_t i = 0; i < wanted.size(); ++i)
        if (!urgent[i]) order.push_back(wanted[i]);
    // The first startable one so ordered, and the startable ones of its
    // level after it, up to a batch.
    const LandRegionKey* first = nullptr;
    for (const LandRegionKey& k : order)
        if (startable(k)) {
            first = &k;
            break;
        }
    if (!first) return false;
    std::vector<LandRegionKey> picks{*first};
    for (const LandRegionKey& k : order) {
        if (static_cast<int>(picks.size()) >= kBatch) break;
        if (k.level == first->level && !(k == *first) && startable(k)) picks.push_back(k);
    }
    Job job;
    for (const LandRegionKey& k : picks) {
        if (land.freeSlots.empty()) {
            // The longest unwanted region in (none that a region being made
            // or read back stands on).
            uint32_t victim = 0;
            uint64_t oldest = land.frame;
            int level = L.config.levels;
            for (const auto& [packed, r] : land.resident) {
                if (!r.in || r.wanted == land.frame) continue;
                bool busy = false;
                for (const auto& [k2, o] : land.resident)
                    if (!o.in && o.key.level < r.key.level && landRegionsOverlap(L, r.key, o.key)) busy = true;
                if (busy) continue;
                if (r.wanted < oldest || (r.wanted == oldest && r.key.level < level)) {
                    victim = packed;
                    oldest = r.wanted;
                    level = r.key.level;
                }
            }
            if (!victim) break;
            drop(node, land, victim);
            if (land.freeSlots.empty()) break;
        }
        Resident r;
        r.key = k;
        r.slot = land.freeSlots.back();
        r.wanted = land.frame;
        land.freeSlots.pop_back();
        land.resident[k.packed()] = r;
        job.members.push_back({k, r.slot});
    }
    if (job.members.empty()) return false;
    land.job = std::move(job);
    land.hasJob = true;
    return true;
}

SceneLandRegions::Step SceneLandRegions::stepOf(const LandRegionConfig& C, Job& j) {
    if (!j.started) return Step::Init;
    while (j.pass < C.passes.size() && j.iteration >= C.passes[j.pass].iterations) {
        ++j.pass;
        j.iteration = 0;
    }
    return j.pass < C.passes.size() ? Step::Pass : Step::Finish;
}

int SceneLandRegions::freeReadbacks(const Land& land) const {
    int n = 0;
    for (bool busy : land.readbackBusy) n += busy ? 0 : 1;
    return n;
}

void SceneLandRegions::writeTable(SceneFrame& frame, const LandNode& node, Land& land, VkCommandBuffer cmd) {
    const int size = node.regionLattice().tableSize();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(size + 2 * LandRegionConfig::kMaxLevels) * 16;
    land.table = {land.tableBuffer.buffer, 0, bytes};
    if (!land.tableDirty) return;
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes, kStorageAlign);
    if (!slice) return;
    land.tableDirty = false;
    auto* e = static_cast<uint32_t*>(slice.mapped);
    std::memset(e, 0, bytes);
    for (const auto& [packed, r] : land.resident) {
        if (!r.in) continue;
        uint32_t h = LandRegionLattice::hash(packed, size);
        for (int k = 0; k < 16; ++k, h = (h + 1) & static_cast<uint32_t>(size - 1))
            if (e[h * 4] == 0) {
                e[h * 4] = packed;
                e[h * 4 + 1] = static_cast<uint32_t>(r.slot);
                break;
            }
    }
    // Each level's centre: (its direction, 1 when it has one), (inner,
    // outer, its chart x, z).
    auto* f = reinterpret_cast<float*>(e + static_cast<size_t>(size) * 4);
    for (int l = 0; l < LandRegionConfig::kMaxLevels; ++l) {
        const LandRegionCentre& c = land.centres[l];
        const float w[8] = {static_cast<float>(c.at.dir[0]), static_cast<float>(c.at.dir[1]),
                            static_cast<float>(c.at.dir[2]), c.active ? 1.0f : 0.0f,
                            static_cast<float>(c.inner),     static_cast<float>(c.outer),
                            static_cast<float>(c.at.x),      static_cast<float>(c.at.z)};
        std::memcpy(f + l * 8, w, sizeof(w));
    }
    // Into the table once what read it last is done, before what reads it next.
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
    const VkBufferCopy copy{slice.offset, 0, bytes};
    vkCmdCopyBuffer(cmd, slice.buffer, land.tableBuffer.buffer, 1, &copy);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

void SceneLandRegions::record(SceneFrame& frame, LandNode& node, Land& land, VkImageView grid, VkCommandBuffer cmd,
                              Step step, int& budget) {
    const LandRegionLattice& L = node.regionLattice();
    const LandRegionConfig& C = L.config;
    Job& j = land.job;
    const size_t n = j.members.size();
    if (!land.hasJob || n == 0 || stepOf(C, j) != step) return;
    const uint32_t cells = static_cast<uint32_t>(C.cells);

    if (step == Step::Finish) {
        // Done: each region's kept channels into its slot, its channel 0 to
        // the CPU.
        if (freeReadbacks(land) < static_cast<int>(n) || budget <= 0) return;
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
        for (size_t m = 0; m < n; ++m) {
            int buffer = 0;
            while (land.readbackBusy[buffer]) ++buffer;
            const SceneVkImage& out = land.run[m][j.current];
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, static_cast<uint32_t>(C.keep)};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(j.members[m].slot * C.keep),
                                   static_cast<uint32_t>(C.keep)};
            copy.extent = {cells, cells, 1};
            vkCmdCopyImage(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, land.slots.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &copy);
            VkBufferImageCopy rb{};
            rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rb.imageExtent = {cells, cells, 1};
            vkCmdCopyImageToBuffer(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, land.readbacks[buffer].buffer, 1, &rb);
            render::cmdBufferBarrier(cmd, land.readbacks[buffer].buffer, 0, VK_WHOLE_SIZE,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            land.readbackBusy[buffer] = true;
            land.reading.push_back({j.members[m].key, buffer, 0, false});
        }
        VkMemoryBarrier a{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        a.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        a.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &a, 0,
                             nullptr, 0, nullptr);
        land.hasJob = false;
        land.job = Job{};
        --budget;
        return;   // (the next batch is begun by the next frame's prepare)
    }

    // Each region's two sets: sets[m][k] reads run[m][k ^ 1], writes run[m][k].
    VkDescriptorSet sets[kBatch][2];
    for (size_t m = 0; m < n; ++m)
        for (int k = 0; k < 2; ++k) {
            sets[m][k] = frame.gpu.device.frameSet(setLayout_);
            if (sets[m][k] == VK_NULL_HANDLE) {
                budget = 0;
                return;
            }
            SceneVkDescriptorWriter writer;
            writer.writeImage(0, land.run[m][k ^ 1].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writer.writeImage(1, land.run[m][k].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writer.writeImage(2, grid, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writer.writeImage(3, land.slots.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writer.writeBuffer(4, land.table.buffer, land.table.range, land.table.offset,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.writeBuffer(5, land.data.buffer, land.data.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            writer.updateSet(device_, sets[m][k]);
        }
    const uint32_t groups = (cells + LP_GROUP - 1) / LP_GROUP;
    const uint32_t bandGroups = (groups + kBands - 1) / kBands;
    // (The run's images: the last copy out of them, or their clear.)
    {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, land.pipeline);
    }
    // One band of a region's cells' rows (-1: all of them).
    const auto dispatch = [&](size_t m, uint32_t mode, int k, uint32_t pass, uint32_t iteration, int band) {
        const LandRegionKey& key = j.members[m].key;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets[m][k], 0, nullptr);
        const uint32_t words[LR_P_WORDS] = {mode,
                                            pass,
                                            iteration,
                                            static_cast<uint32_t>(key.level),
                                            static_cast<uint32_t>(key.root),
                                            static_cast<uint32_t>(key.i),
                                            static_cast<uint32_t>(key.j),
                                            band < 0 ? 0u : static_cast<uint32_t>(band) * bandGroups * LP_GROUP};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(words), words);
        vkCmdDispatch(cmd, groups,
                      band < 0 ? groups : std::min(bandGroups, groups - static_cast<uint32_t>(band) * bandGroups), 1);
    };

    // Each step over every region of the batch, then its one barrier (the
    // init's bands write apart, and only its last one is waited on).
    while (budget > 0 && stepOf(C, j) == step) {
        if (step == Step::Init) {
            for (size_t m = 0; m < n; ++m) dispatch(m, LP_MODE_INIT, 0, 0, 0, j.band);
            --budget;
            if (++j.band == kBands) {
                computeBarrier(cmd);
                j.band = 0;
                j.started = true;
                j.current = 0;
            }
            continue;
        }
        for (size_t m = 0; m < n; ++m)
            dispatch(m, LP_MODE_PASS, j.current ^ 1, static_cast<uint32_t>(j.pass), static_cast<uint32_t>(j.iteration),
                     -1);
        computeBarrier(cmd);
        --budget;
        ++j.iteration;
        j.current ^= 1;
    }
}

SceneLandRegions::Offer SceneLandRegions::prepare(SceneFrame& frame, LandNode& node, VkImageView grid,
                                                  VkCommandBuffer cmd) {
    const LandRegionLattice& L = node.regionLattice();
    if (!L.config.active() || !grid || !node.ready()) {
        if (!L.config.active()) forget(frame.gpu, node.id());
        node.setRegionsSettled(!L.config.active());
        return {};
    }
    auto& slot = lands_[node.id()];
    if (slot && slot->serial != node.gridSerial()) {
        release(frame.gpu, *slot);
        slot.reset();
    }
    if (!slot) {
        // (Not started until its kernels are compiled: the regions are not
        // settled.)
        bool pending = false;
        pipeline(node, &pending);
        if (pending) {
            lands_.erase(node.id());
            node.setRegionsSettled(false);
            return {};
        }
        slot = std::make_unique<Land>();
        slot->serial = node.gridSerial();
        if (!start(frame.gpu, node, *slot)) {
            LOG_ERROR("Land node %u: its regions could not be made", node.id());
            slot->failed = true;
        }
    }
    Land& land = *slot;
    if (land.failed) {
        node.setRegionsSettled(true);
        return {};
    }
    ++land.frame;
    receive(frame, node, land);

    // What is wanted about the eye: its body direction, its height over the
    // ground, and the view's focal length.
    const SceneView& view = frame.view;
    double eye[3];
    node.place()->toBody(view.eye, eye);
    double dir[3] = {0.0, 1.0, 0.0}, height = 0.0;
    if (L.round()) {
        const double r = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
        for (int i = 0; i < 3; ++i) dir[i] = eye[i] / std::max(r, 1.0);
        height = r - L.radius - std::max(static_cast<double>(node.heightOver(dir)), 0.0);
    } else {
        const float h = node.heightAt(view.eye.x, view.eye.z);
        height = view.eye.y - (std::isfinite(h) ? h : 0.0);
    }
    const double focal = 0.5 * std::max(view.height, 1u) * std::fabs(view.proj.at(1, 1));
    std::vector<LandRegionKey>& wanted = land.wanted;
    std::vector<LandRegionKey> near;
    // Each wanted level's disc about the ground under the eye (a level no
    // longer wanted keeps its last, while its regions stay).
    LandGroundPoint ground;
    for (int i = 0; i < 3; ++i) ground.dir[i] = dir[i];
    ground.x = eye[0];
    ground.z = eye[2];
    LandGroundPoint targets[LandRegionConfig::kMaxLevels];
    for (int l = 0; l < L.config.levels; ++l) {
        if (L.levelWanted(l, std::max(height, 1.0), focal * kLead)) land.fades[l].aim(L, l, ground);
        targets[l] = land.fades[l].target;
    }
    landRegionsWanted(L, dir, eye[0], eye[2], std::max(height, 1.0), focal * kLead, wanted, &near, targets);
    land.pending = 0;
    for (const LandRegionKey& k : wanted) {
        const auto r = land.resident.find(k.packed());
        if (r != land.resident.end()) r->second.wanted = land.frame;
        if (r == land.resident.end() || !r->second.in) ++land.pending;
    }
    bool settled = true;
    for (const LandRegionKey& k : near) {
        const auto r = land.resident.find(k.packed());
        if (r == land.resident.end() || !r->second.in) settled = false;
    }
    const bool nearIn = settled;

    // Work while a region is being made or one can be begun; then the
    // table, after begin: a region it dropped for room stales the tiles over
    // it, made again this frame, and they must not find it in the table
    // (they would keep its heights after it is gone).
    const bool working = land.hasJob || begin(node, land, wanted, near);
    // Each level read in the disc its regions in cover (after begin: a
    // region it dropped is not); where that changes, the items over it are
    // made again.
    bool faded = false;
    for (int l = 0; l < L.config.levels; ++l) {
        const LandRegionCentre c = land.fades[l].step(L, l, [&](const LandRegionKey& k) {
            const auto r = land.resident.find(k.packed());
            return r != land.resident.end() && r->second.in;
        });
        if (c == land.centres[l]) continue;
        node.regionFadeChanged(land.centres[l], c);
        land.centres[l] = c;
        faded = true;
    }
    if (faded) {
        land.tableDirty = true;
        node.regionStore().setCentres(land.centres);
    }
    // (Settled once, too, every wanted level is read whole at the ground
    // under the eye: its disc grown past it.)
    for (int l = 0; l < L.config.levels && settled; ++l)
        if (L.levelWanted(l, std::max(height, 1.0), focal * kLead) &&
            L.presence(land.centres[l], dir, eye[0], eye[2]) < 1.0)
            settled = false;
    node.setRegionsSettled(settled);
    writeTable(frame, node, land, cmd);
    if (!land.table.buffer || !working) return {};
    // (While the ground under the eye is not in, all of them required, the
    // offerer bounding them to kUrgentMs of the frame: the land is wrong
    // there, and the plants wait on it. Else kRequiredSteps a frame whatever the budget, so the
    // regions about a moving eye keep coming in at the floor, the rest as
    // the budget holds them, as pressing as the pixels a cell of the
    // level being made spans, as a tile's texels; the coarser levels' field
    // is read where a region is not yet in.)
    // A frame offers the batch's next kind of step only (its init's bands,
    // its passes, or its finish once read-backs are free for it).
    Job& j = land.job;
    Offer o;
    o.step = stepOf(L.config, j);
    o.regions = static_cast<int>(j.members.size());
    o.error = static_cast<float>(L.config.cellOf(j.members.front().key.level) * focal / std::max(height, 1.0));
    o.urgent = !nearIn;
    if (o.step == Step::Finish) {
        if (freeReadbacks(land) < o.regions) return {};
        o.steps = o.required = 1;
        return o;
    }
    int left = kBands - j.band;
    if (o.step == Step::Pass) {
        left = L.config.passes[j.pass].iterations - j.iteration;
        for (size_t p = j.pass + 1; p < L.config.passes.size(); ++p) left += L.config.passes[p].iterations;
    }
    o.steps = std::min(left, o.urgent ? kDispatchesWarming : kDispatchesPerFrame);
    // (Else kRequiredSteps regions' steps: one step of a batch of four is
    // already four of them, and two such of its dearest pass were a
    // millisecond a frame whatever the budget.)
    const int required = o.step == Step::Pass ? std::max(1, kRequiredSteps / std::max(o.regions, 1)) : 1;
    o.required = o.urgent ? o.steps : std::min(o.steps, required);
    return o;
}

int SceneLandRegions::run(SceneFrame& frame, LandNode& node, VkImageView grid, VkCommandBuffer cmd, Step step,
                          int budget) {
    const auto it = lands_.find(node.id());
    if (it == lands_.end() || it->second->failed || !grid || !it->second->table.buffer) return 0;
    Land& land = *it->second;
    const int regions = static_cast<int>(land.job.members.size());
    const int given = budget;
    record(frame, node, land, grid, cmd, step, budget);
    return (given - budget) * regions;
}

void SceneLandRegions::submitted(uint64_t ticket) {
    for (auto& [id, land] : lands_)
        for (Reading& r : land->reading)
            if (!r.ticketed) {
                r.ticket = ticket;
                r.ticketed = true;
            }
}

}  // namespace bro::scene::vk

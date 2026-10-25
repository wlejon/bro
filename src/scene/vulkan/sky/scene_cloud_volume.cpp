#include "scene/vulkan/sky/scene_cloud_volume.h"

#include "render/vulkan_util.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bro::scene::vk {

// (cloudVolSample's and cloudVolTexel's branches, sky/cloud_volume.glsl.)
static_assert(CL_VOL_LEVELS == 5);
static_assert((CL_VOL_SIDE & (CL_VOL_SIDE - 1)) == 0 && (CL_VOL_DEPTH & (CL_VOL_DEPTH - 1)) == 0);
static_assert(CL_VOL_SIDE % CL_VOL_SLICES == 0 && CL_VOL_CHECK_RAYS % 64 == 0);

namespace {

constexpr VkFormat kVolFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDistFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
// How far (m, along the deck) the eye may be from the anchor before the
// frame is made again: the deck's sag below the frame's plane there and a
// coarsest window's half beyond it (some 600 m) stays inside its depth.
constexpr double kAnchorReach = 40000.0;
// The check: a frame in this many, while BRO_CLOUD_VOLUME_CHECK is set.
constexpr uint32_t kCheckEvery = 30;
constexpr VkDeviceSize kCheckBytes = sizeof(float) * CL_VOL_CHECK_RAYS * CL_VOL_CHECK_FLOATS;
constexpr int kDepth[3] = {CL_VOL_SIDE, CL_VOL_SIDE, CL_VOL_DEPTH};
// The slices baked, from a cache that no longer changes, before what the
// volume holds is final: one round. (A cell's extinction and occupancy are
// its own; its distance reads the rows out to the reach on either side,
// which each slice's distance passes make again about it, so the round's
// last slice makes its neighbour's, the round's first, again.)
constexpr int kSettleSlices = CL_VOL_SLICES;

double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
             VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

struct DebugEnv {
    int view = 0;
    float height = 1000.0f;
};
const DebugEnv& debugEnv() {
    static const DebugEnv env = [] {
        DebugEnv d;
        if (const char* s = std::getenv("BRO_CLOUD_VOLUME_DEBUG")) {
            d.view = std::clamp(std::atoi(s), 0, 3);
            if (const char* c = std::strchr(s, ':')) d.height = static_cast<float>(std::atof(c + 1));
        }
        return d;
    }();
    return env;
}
bool checkWanted() {
    static const bool on = [] {
        const char* s = std::getenv("BRO_CLOUD_VOLUME_CHECK");
        return s && *s && *s != '0';
    }();
    return on;
}

}  // namespace

int SceneCloudVolume::debugView() { return debugEnv().view; }
float SceneCloudVolume::debugHeight() { return debugEnv().height; }

bool SceneCloudVolume::setup(SceneGpu& gpu, VkPipelineLayout layout) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    forms_.start(dev, BuiltinSceneShader::CloudsVolumeComp, layout, "clouds volume");
    dist_.start(dev, BuiltinSceneShader::CloudsVolumeDistComp, layout, "clouds volume dist");
    if (checkWanted()) check_.start(dev, BuiltinSceneShader::CloudsVolumeCheckComp, layout, "clouds volume check");
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // (A level's window wraps on every axis.)
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.maxLod = 0.0f;
    if (vkCreateSampler(dev, &s, nullptr, &sampler_) != VK_SUCCESS) sampler_ = VK_NULL_HANDLE;
    return sampler_ != VK_NULL_HANDLE;
}

SceneCloudVolume::Readiness SceneCloudVolume::readiness() {
    Readiness r = Readiness::Ready;
    for (AsyncComputePipeline* p : {&forms_, &dist_, &check_}) {
        if (p == &check_ && !checkWanted()) continue;
        if (!p->get()) {
            if (!p->usable()) return Readiness::Failed;
            r = Readiness::Building;
        }
    }
    return r;
}

void SceneCloudVolume::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (int k = 0; k < CL_VOL_LEVELS; ++k)
        for (SceneVkImage* i : {&vol_[k], &distA_[k], &distB_[k]})
            if (i->isValid()) gpu.allocator.destroyImage(*i);
    if (checkBuf_.isValid()) gpu.allocator.destroyBuffer(checkBuf_);
    for (AsyncComputePipeline* p : {&forms_, &dist_, &check_}) p->destroy(dev);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    anchored_ = baked_ = false;
    pending_ = 0;
    checkRecorded_ = checkPending_ = false;
}

bool SceneCloudVolume::ensureImages(SceneGpu& gpu) {
    if (vol_[0].isValid()) return true;
    SceneVkAllocator& a = gpu.allocator;
    bool ok = a.createBuffer(kCheckBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, checkBuf_) &&
              checkBuf_.mappedData;
    for (int k = 0; ok && k < CL_VOL_LEVELS; ++k)
        ok = a.createStorageImage(CL_VOL_SIDE, CL_VOL_SIDE, CL_VOL_DEPTH, kVolFormat, vol_[k], false) &&
             a.createStorageImage(CL_VOL_SIDE, CL_VOL_SIDE, CL_VOL_DEPTH, kDistFormat, distA_[k], false) &&
             a.createStorageImage(CL_VOL_SIDE, CL_VOL_SIDE, CL_VOL_DEPTH, kDistFormat, distB_[k], false);
    if (!ok) {
        LOG_ERROR("SceneCloudVolume: failed creating the volume's images");
        cleanup(gpu);
    }
    return ok;
}

void SceneCloudVolume::write(SceneVkDescriptorWriter& w) const {
    for (uint32_t k = 0; k < CL_VOL_LEVELS; ++k) {
        w.writeImage(CL_B_VOLUME, vol_[k].view, sampler_, VK_IMAGE_LAYOUT_GENERAL,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k);
        w.writeImage(CL_B_VOLUME_OUT, vol_[k].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k);
        w.writeImage(CL_B_VOLUME_DA, distA_[k].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k);
        w.writeImage(CL_B_VOLUME_DB, distB_[k].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k);
    }
    w.writeBuffer(CL_B_VOLUME_CHECK, checkBuf_.buffer, kCheckBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

void SceneCloudVolume::place(const Frame& f, CloudVolumeGpu& out, CloudStats& stats) {
    boxes_.clear();
    // (A check recorded into a frame that never got submitted is dropped.)
    checkRecorded_ = false;
    readCheck(stats);

    // The eye in the drifted frame.
    double pe[3];
    for (int i = 0; i < 3; ++i) pe[i] = f.eye[i] - static_cast<double>(f.wind[i]) * f.windTime;
    const double Rb = f.Rb, Rt = f.Rt;
    const auto local = [&](double* v) {
        const double d[3] = {pe[0] - A_[0], pe[1] - A_[1], pe[2] - A_[2]};
        v[0] = dot3(d, E1_);
        v[1] = dot3(d, E2_);
        v[2] = dot3(d, N_);
    };
    bool all = !anchored_ || f.key != key_ || f.jump;
    key_ = f.key;
    double ve[3] = {};
    if (anchored_) local(ve);
    if (!anchored_ || std::hypot(ve[0], ve[1]) > kAnchorReach) {
        // The frame: on the deck's bottom under the eye, x and y along it.
        const double l = std::sqrt(dot3(pe, pe));
        for (int i = 0; i < 3; ++i) N_[i] = pe[i] / l;
        double a[3] = {1.0, 0.0, 0.0};
        if (std::fabs(N_[0]) > 0.9) a[0] = 0.0, a[2] = 1.0;
        const double an = dot3(a, N_);
        for (int i = 0; i < 3; ++i) E1_[i] = a[i] - an * N_[i];
        const double l1 = std::sqrt(dot3(E1_, E1_));
        for (double& v : E1_) v /= l1;
        E2_[0] = N_[1] * E1_[2] - N_[2] * E1_[1];
        E2_[1] = N_[2] * E1_[0] - N_[0] * E1_[2];
        E2_[2] = N_[0] * E1_[1] - N_[1] * E1_[0];
        for (int i = 0; i < 3; ++i) A_[i] = N_[i] * Rb;
        anchored_ = true;
        baked_ = false;   // (every cell of every level is elsewhere)
        all = true;
        local(ve);
    }

    // The levels' windows about the eye's cell.
    const bool whole = all && !baked_;
    const double rho = std::hypot(ve[0], ve[1]);
    for (int k = 0; k < CL_VOL_LEVELS; ++k) {
        const double cell = CL_VOL_CELL0 * std::ldexp(1.0, k);
        // (A window moves once the eye is a quarter cell past the cell it is
        // about, so an eye on a cell's edge does not move it back and forth.)
        const auto follow = [&](double u, long long c) {
            const long long fl = static_cast<long long>(std::floor(u));
            const bool near = u >= static_cast<double>(c) - 0.25 && u < static_cast<double>(c) + 1.25;
            return whole || !baked_ || !near ? fl : c;
        };
        // Up: the deck over the window's reach across (its bottom sags below
        // the frame's plane away from the anchor, its top highest nearest).
        const double half = 0.5 * CL_VOL_SIDE * cell * std::sqrt(2.0);
        const double rFar = rho + half, rNear = std::max(rho - half, 0.0);
        const double zb = rFar < Rb ? std::sqrt(Rb * Rb - rFar * rFar) - Rb : -(Rt - Rb);
        const double zt = std::sqrt(std::max(Rt * Rt - rNear * rNear, 0.0)) - Rb;
        const double Z = CL_VOL_DEPTH * cell;
        const double zc = Z >= zt - zb + 2.0 * cell ? 0.5 * (zb + zt)
                                                    : std::clamp(ve[2], zb + 0.5 * Z - cell, zt - 0.5 * Z + cell);
        const long long c[3] = {follow(ve[0] / cell, centre_[k][0]), follow(ve[1] / cell, centre_[k][1]),
                                follow(zc / cell, centre_[k][2])};
        for (int axis = 0; axis < 3; ++axis) {
            if (!whole && baked_) moveLevel(k, axis, centre_[k][axis], c[axis]);
            centre_[k][axis] = c[axis];
            out.level[k][axis] = static_cast<float>(c[axis]);
        }
        out.level[k][3] = 1.0f;
    }
    // The bakes: everything the first frame; else a level whole a frame
    // (coarsest first) while any is to be baked again, and a slice of every
    // level's rows.
    const auto level = [](int k) { return Box{k, 0, CL_VOL_SIDE, 0, CL_VOL_SIDE, 0, CL_VOL_DEPTH, 0}; };
    if (whole) {
        boxes_.clear();
        for (int k = 0; k < CL_VOL_LEVELS; ++k) boxes_.push_back(level(k));
        pending_ = 0;
        baked_ = true;
        for (float& t : sliceTime_) t = f.windTime;
    } else {
        if (all) pending_ = (1u << CL_VOL_LEVELS) - 1u;
        if (pending_) {
            int k = CL_VOL_LEVELS - 1;
            while (!(pending_ & (1u << k))) --k;
            pending_ &= ~(1u << k);
            boxes_.push_back(level(k));
        }
        const int j = static_cast<int>(frame_ % CL_VOL_SLICES), rows = CL_VOL_SIDE / CL_VOL_SLICES;
        for (int k = 0; k < CL_VOL_LEVELS; ++k) {
            Box b = level(k);
            b.y0 = j * rows;
            b.ny = rows;
            boxes_.push_back(b);
        }
        sliceTime_[j] = f.windTime;
    }
    ++frame_;
    // What is still to come of it for this view (the settle state): the
    // levels to bake whole again, and a round of every slice since the last
    // whole change or the last frame the cache it is baked from still
    // changed (kSettleSlices).
    sliceRun_ = all || !f.sourceSettled ? 0 : std::min(sliceRun_ + 1, kSettleSlices);

    // The slack: how far the base may have drifted under the forms since
    // the oldest slice was baked (and a float's rounding of a planet's
    // radius, half a metre, twice).
    float oldest = f.windTime;
    for (float t : sliceTime_) oldest = std::fabs(f.windTime - t) > std::fabs(f.windTime - oldest) ? t : oldest;
    const float wind = std::sqrt(f.wind[0] * f.wind[0] + f.wind[1] * f.wind[1] + f.wind[2] * f.wind[2]);
    const float slack = wind * std::fabs(f.windTime - oldest) + 1.0f;

    for (int i = 0; i < 3; ++i) {
        out.A[i] = static_cast<float>(A_[i]);
        out.E1[i] = static_cast<float>(E1_[i]);
        out.E2[i] = static_cast<float>(E2_[i]);
        out.N[i] = static_cast<float>(N_[i]);
        out.eye[i] = static_cast<float>(ve[i]);
    }
    out.A[3] = static_cast<float>(CL_VOL_CELL0);
    out.E1[3] = slack;
    out.E2[3] = static_cast<float>(debugView());
    out.N[3] = debugHeight();
    out.eye[3] = f.pixel;

    cellsBaked_ = 0;
    for (const Box& b : boxes_)
        if (b.pass0 == 0) cellsBaked_ += static_cast<uint64_t>(b.nx) * b.ny * b.nz;
    stats.volumePending = std::popcount(pending_) + (kSettleSlices - sliceRun_);
    stats.volumeBytes = static_cast<double>(CL_VOL_LEVELS) * CL_VOL_SIDE * CL_VOL_SIDE * CL_VOL_DEPTH * (8 + 2 + 2) +
                        static_cast<double>(kCheckBytes);
    stats.volumeCells = static_cast<double>(cellsBaked_);
    stats.volumeSlack = slack;
}

void SceneCloudVolume::moveLevel(int k, int axis, long long from, long long to) {
    const long long d = to - from;
    if (d == 0) return;
    const int n = kDepth[axis], H = n / 2, R = CL_VOL_REACH;
    if (std::llabs(d) >= H) {
        // (A jump past half the window: little of it is kept.)
        boxes_.push_back(Box{k, 0, CL_VOL_SIDE, 0, CL_VOL_SIDE, 0, CL_VOL_DEPTH, 0});
        return;
    }
    // Cells `g0` .. `g0 + count - 1` along the axis, from pass `pass0` on.
    const auto band = [&](long long g0, int count, int pass0) {
        Box b{k, 0, CL_VOL_SIDE, 0, CL_VOL_SIDE, 0, CL_VOL_DEPTH, pass0};
        const int s0 = static_cast<int>(g0 & (n - 1));
        if (axis == 0) b.x0 = s0, b.nx = count;
        else if (axis == 1) b.y0 = s0, b.ny = count;
        else b.z0 = s0, b.nz = count;
        boxes_.push_back(b);
    };
    // The cells taken in (their distances, and those out to the reach
    // inside the old edge, by the distance passes' growth); and at the edge
    // given up, the distances out to the reach that counted on what was
    // given up.
    const int m = static_cast<int>(std::llabs(d));
    if (d > 0) {
        band(from + H, m, 0);
        band(to - H, R, 1);
    } else {
        band(to - H, m, 0);
        band(to + H - R, R, 1);
    }
}

void SceneCloudVolume::record(VkCommandBuffer cmd, VkDescriptorSet set, VkPipelineLayout layout) {
    const bool check = checkWanted() && !checkPending_ && frame_ % kCheckEvery == 0;
    if ((boxes_.empty() && !check) || !set) return;
    // What earlier frames read of the volume is done with; the cache baked.
    barrier(cmd, kReaders, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const int R = CL_VOL_REACH;
    const auto grow = [R](int& x0, int& count, int side) {
        if (count + 2 * R >= side) {
            x0 = 0;
            count = side;
        } else {
            x0 = (x0 - R) & (side - 1);
            count += 2 * R;
        }
    };
    // The forms, then the distance an axis a pass (each over its boxes
    // grown by the reach along the axes done so far).
    for (int pass = 0; pass < 4 && !boxes_.empty(); ++pass) {
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass == 0 ? forms_.get() : dist_.get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        for (const Box& b0 : boxes_) {
            if (pass < b0.pass0) continue;
            Box b = b0;
            if (pass >= 1) grow(b.x0, b.nx, CL_VOL_SIDE);
            if (pass >= 2) grow(b.y0, b.ny, CL_VOL_SIDE);
            if (pass == 3) grow(b.z0, b.nz, CL_VOL_DEPTH);
            const int32_t push[8] = {b.k, b.x0, b.nx, std::max(pass - 1, 0), b.y0, b.ny, b.z0, b.nz};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            const auto groups = [](int count) { return static_cast<uint32_t>((count + CL_VOL_GROUP - 1) / CL_VOL_GROUP); };
            vkCmdDispatch(cmd, groups(b.nx), groups(b.ny), groups(b.nz));
        }
        barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                pass < 3 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : kReaders, VK_ACCESS_SHADER_READ_BIT);
    }
    if (check) {
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, check_.get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const int32_t push[8] = {static_cast<int32_t>(++checkSerial_), 0, 0, 0, 0, 0, 0, 0};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, CL_VOL_CHECK_RAYS / 64, 1, 1);
        barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        checkRecorded_ = true;
    }
}

void SceneCloudVolume::submitted(uint64_t ticket) {
    if (!checkRecorded_) return;
    checkRecorded_ = false;
    checkPending_ = true;
    checkTicket_ = ticket;
}

void SceneCloudVolume::readCheck(CloudStats& stats) {
    if (checkPending_ && device_ && device_->context().queue().isComplete(checkTicket_)) {
        checkPending_ = false;
        const float* r = static_cast<const float*>(checkBuf_.mappedData);
        double s[CL_VOL_CHECK_FLOATS] = {};
        double most = 0.0;
        for (int i = 0; i < CL_VOL_CHECK_RAYS; ++i) {
            const float* q = r + i * CL_VOL_CHECK_FLOATS;
            for (int j = 0; j < CL_VOL_CHECK_FLOATS; ++j) s[j] += q[j];
            most = std::max(most, static_cast<double>(q[3]));
        }
        CloudStats::VolumeCheck& c = lastCheck_;
        c.serial = checkSerial_;
        c.rays = CL_VOL_CHECK_RAYS;
        c.compared = s[2];
        c.meanAbsError = s[2] > 0.0 ? s[0] / s[2] : 0.0;
        c.meanSigma = s[2] > 0.0 ? s[1] / s[2] : 0.0;
        c.mostSigma = most;
        c.violations = s[4];
        c.inside = s[5];
        c.jumps = s[6];
        c.meanJump = s[6] > 0.0 ? s[7] / s[6] : 0.0;
        LOG_INFO("cloud volume check %u: %.0f points, extinction error %.3g (forms' mean %.3g, most %.3g /m); "
                 "%.0f jumps, mean %.0f m, %.0f points inside, %.0f violations",
                 c.serial, c.compared, c.meanAbsError, c.meanSigma, c.mostSigma, c.jumps, c.meanJump, c.inside,
                 c.violations);
    }
    stats.volumeCheck = lastCheck_;
}

}  // namespace bro::scene::vk

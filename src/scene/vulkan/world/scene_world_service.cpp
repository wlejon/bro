#include "scene/vulkan/world/scene_world_service.h"

#include "scene/nodes/land_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_cpu_stalls.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>

namespace bro::scene::vk {

static_assert(WorldService::kMaxKinds <= static_cast<int>(SceneGpuTimer::kMaxSpans),
              "each world job kind is one span of the GPU timer");

VkCommandBuffer WorldStream::cmd() {
    if (cmd_ != VK_NULL_HANDLE) return cmd_;
    if (schedule_ == WorldSchedule::Compute && streams_->compute) {
        cmd_ = streams_->compute();
        apart_ = cmd_ != streams_->graphics;
    } else {
        cmd_ = streams_->graphics;
    }
    if (streams_->timer) stamped_ = streams_->timer->beginSpan(cmd_, static_cast<uint32_t>(kind_), apart_);
    return cmd_;
}

SceneWorldService::SceneWorldService() = default;

int SceneWorldService::addKind(WorldProducer* producer, const WorldJobKind& kind) {
    const int k = policy_.addKind(kind);
    if (k < 0) {
        LOG_ERROR("SceneWorldService: no room for the job kind '%s' (WorldService::kMaxKinds is %d)",
                  kind.name.c_str(), WorldService::kMaxKinds);
        return -1;
    }
    producers_.resize(static_cast<size_t>(k) + 1, nullptr);
    producers_[static_cast<size_t>(k)] = producer;
    markNames_[static_cast<size_t>(k)] = "world:" + kind.name;
    offerNames_[static_cast<size_t>(k)] = "world offer:" + kind.name;
    return k;
}

bool SceneWorldService::viewCut(const bromath::Vec3& eye, const bromath::Vec3& forward, const bromath::Vec3& wasEye,
                                const bromath::Vec3& wasForward, float halfFov, float above) {
    const bromath::Vec3 d = eye - wasEye;
    const float far = std::max(kCutMetres, std::max(above, 0.0f) * std::tan(halfFov));
    if (bromath::vdot(d, d) > far * far) return true;
    const float n = std::sqrt(bromath::vdot(forward, forward) * bromath::vdot(wasForward, wasForward));
    return n > 0.0f && bromath::vdot(forward, wasForward) < n * std::cos(halfFov);
}

void SceneWorldService::beginFrame(SceneGpuTimer* timer, float targetMs, bool cut) {
    if (cut) {
        policy_.cut();
        if (timer) staleThrough_ = timer->submittedRenders();
    }
    if (timer) {
        std::vector<SceneGpuTimer::Frame> frames;
        timer->takeFrames(frames);
        for (const SceneGpuTimer::Frame& f : frames) {
            WorldFrameTiming t;
            t.frameMs = f.order > staleThrough_ ? f.ms : -1.0;   // (a view before the cut: its costs only)
            for (const SceneGpuTimer::Frame::Span& s : f.spans)
                t.spans.push_back({static_cast<int>(s.id), s.units, s.ms, s.apart});
            policy_.observe(t);
        }
    }
    policy_.setTarget(targetMs);
    policy_.beginFrame();
}

double SceneWorldService::run(WorldStreams& streams, int kind, const std::function<double(WorldStream&)>& record) {
    if (kind < 0 || kind >= policy_.kindCount()) return 0.0;
    WorldStream stream;
    stream.streams_ = &streams;
    stream.kind_ = kind;
    stream.schedule_ = policy_.kind(kind).schedule;
    const double units = record(stream);
    if (stream.stamped_) streams.timer->endSpan(stream.cmd_, static_cast<uint32_t>(kind), stream.apart_, units);
    // Each kind on the graphics queue its own part of the timed passes.
    if (streams.timer && stream.cmd_ != VK_NULL_HANDLE && !stream.apart_)
        streams.timer->mark(stream.cmd_, markNames_[static_cast<size_t>(kind)].c_str());
    return units;
}

void SceneWorldService::runFrame(SceneFrame& frame) {
    // The view's eye and forward (the camera looks down its -z), and whether
    // it jumped since the last render (on the body: a re-anchor is no cut).
    const SceneView& view = frame.world;
    const bromath::Vec3 forward{-view.invRelView.at(0, 2), -view.invRelView.at(1, 2), -view.invRelView.at(2, 2)};
    const float halfFov = view.perspective ? std::atan(1.0f / std::max(std::fabs(view.proj.at(1, 1)), 1e-6f))
                                           : 0.5235988f;   // (an orthographic view: 30 degrees)
    // (How high the eye stands: over the first land's ground.)
    float above = 0.0f;
    for (const auto& [id, node] : frame.graph.nodes())
        if (node->type() == SceneNode::Type::Mesh)
            if (const LandNode* land = static_cast<MeshNode*>(node.get())->asLand()) {
                above = land->eyeAbove(view.eye);
                break;
            }
    const bool cut = haveView_ && viewCut(view.eye, forward, eye_, forward_, halfFov, above);
    eye_ = view.eye;
    forward_ = forward;
    haveView_ = true;
    {
        stalls::Part part("world:costs");
        beginFrame(frame.timer, frame.renderer.frameTargetMs(), cut);
    }
    // Each producer offers once, whatever kinds it registered.
    std::vector<WorldProducer*> offered;
    for (size_t k = 0; k < producers_.size(); ++k) {
        WorldProducer* p = producers_[k];
        if (!p || std::find(offered.begin(), offered.end(), p) != offered.end()) continue;
        offered.push_back(p);
        // (Named in a slow render's log by its first kind.)
        stalls::Part part(offerNames_[k].c_str());
        p->offer(frame, policy_);
    }
    {
        stalls::Part part("world:schedule");
        policy_.schedule();
    }
    WorldStreams streams{frame.cmd, [&frame] { return frame.computeStream(); }, frame.timer};
    for (int k = 0; k < policy_.kindCount(); ++k) {
        WorldProducer* p = producers_[static_cast<size_t>(k)];
        if (!p) continue;
        stalls::Part part(markNames_[static_cast<size_t>(k)].c_str());
        run(streams, k, [&](WorldStream& s) { return p->record(frame, k, policy_.granted(k), s); });
    }
    const WorldServiceStats& s = policy_.stats();
    frame.stats.worldBudgetMs = s.budgetMs;
    frame.stats.worldSpentMs = s.spentMs;
    frame.stats.worldJobs = static_cast<int>(s.granted);
    frame.stats.worldQueued = static_cast<int>(s.queued);
    frame.stats.worldWorstError = s.worstError;
    frame.stats.worldKindMs.clear();
    for (int k = 0; k < policy_.kindCount(); ++k) frame.stats.worldKindMs.emplace_back(policy_.kind(k).name, policy_.cost(k));
    frame.stats.worldKindQueued.clear();
    for (int k = 0; k < policy_.kindCount(); ++k)
        if (policy_.queued(k))
            frame.stats.worldKindQueued.push_back(
                {policy_.kind(k).name, static_cast<int>(policy_.queued(k)), policy_.worstError(k)});
}

}  // namespace bro::scene::vk

// Query objects. ANY_SAMPLES_PASSED(_CONSERVATIVE) is a Vulkan occlusion
// query: one is open for each render pass instance while the GL query is
// active (a Vulkan query cannot outlive the pass it began in, nor the command
// buffer), and the GL result is whether any of them counted a sample.
// TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN counts on the CPU, since the draws
// that capture know what they write. EXT_disjoint_timer_query_webgl2's
// TIME_ELAPSED_EXT is a pair of Vulkan timestamps written as the work before
// each finishes (so a run of queries splits the GPU's time between them), and
// queryCounterEXT's TIMESTAMP_EXT one. A result is available once the
// submission holding the query's last use has completed; nothing but
// QUERY_RESULT itself waits for it.

#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <vector>

namespace bro::webgl::vk {

namespace {

constexpr uint32_t kSlotsPerPool = 64;

bool occlusionTarget(GLenum target) {
    return target == GL_ANY_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE;
}

// The samples the slots counted, added to `count`; false when one has no
// result yet. A completed submission is not enough: MoltenVK copies Metal's
// visibility results into the pool in the command buffer's completion
// handler, which can run after the submission's ticket reads complete, and a
// slot answers VK_NOT_READY in between (~2 runs in 100 of
// tests/webgl/test_webgl_query.js read a fullscreen draw as 0 samples). With
// `wait` the read waits that out, which takes no longer than the handler.
bool readSlots(VkDevice device, const std::vector<VkQueryPool>& pools, const std::vector<uint32_t>& slots,
               bool wait, std::vector<uint64_t>& values) {
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
    if (wait) flags |= VK_QUERY_RESULT_WAIT_BIT;
    values.clear();
    for (uint32_t slot : slots) {
        uint64_t value = 0;
        if (vkGetQueryPoolResults(device, pools[slot / kSlotsPerPool], slot % kSlotsPerPool, 1, sizeof(value),
                                  &value, sizeof(value), flags) != VK_SUCCESS)
            return false;
        values.push_back(value);
    }
    return true;
}

bool readOcclusionSlots(VkDevice device, const std::vector<VkQueryPool>& pools,
                        const std::vector<uint32_t>& slots, bool wait, uint64_t& count) {
    std::vector<uint64_t> samples;
    if (!readSlots(device, pools, slots, wait, samples)) return false;
    for (uint64_t s : samples) count += s;
    return true;
}

} // namespace

GLuint WebGLVkContext::createQuery() {
    const GLuint id = nextObjectId_++;
    queries_[id] = QueryObject{};
    return id;
}

GLboolean WebGLVkContext::isQuery(GLuint id) const {
    auto it = queries_.find(id);
    return it != queries_.end() && it->second.target != 0 ? GL_TRUE : GL_FALSE;
}

void WebGLVkContext::deleteQuery(GLuint id) {
    auto it = queries_.find(id);
    if (it == queries_.end()) return;
    // Deleting an active query ends it.
    if (activeOcclusionQuery_ == id) endQuery(it->second.target);
    if (activeFeedbackQuery_ == id) endQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    if (activeTimerQuery_ == id) endQuery(GL_TIME_ELAPSED_EXT);
    retireQuerySlots(it->second);
    queries_.erase(it);
}

// Hand slots back once the GPU is done with them: after the submission that
// last used them, or the next one if that is the open command buffer.
void WebGLVkContext::retireSlots(QuerySlots& slots, uint64_t ticket, std::vector<uint32_t>& taken) {
    if (taken.empty()) return;
    slots.retired.push_back({ticket, std::move(taken)});
    taken.clear();
}

void WebGLVkContext::retireQuerySlots(QueryObject& q) {
    const uint64_t ticket = q.pending ? kUnsubmittedTicket : q.ticket;
    retireSlots(occlusionSlots_, ticket, q.slots);
    retireSlots(timestampSlots_, ticket, q.stamps);
}

bool WebGLVkContext::timerQueriesSupported() const {
    return timestampBits_ > 0 && timestampPeriodNs_ > 0.0f;
}

void WebGLVkContext::beginQuery(GLenum target, GLuint id) {
    const bool timer = target == GL_TIME_ELAPSED_EXT && timerQueriesEnabled_;
    if (!occlusionTarget(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN && !timer) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    auto it = queries_.find(id);
    GLuint& active = occlusionTarget(target) ? activeOcclusionQuery_
                     : timer                 ? activeTimerQuery_
                                             : activeFeedbackQuery_;
    // One query per target at a time, the two occlusion targets sharing one
    // slot; a query keeps the target it was first begun with.
    if (it == queries_.end() || active != 0 || id == activeOcclusionQuery_ || id == activeFeedbackQuery_ ||
        id == activeTimerQuery_ || (it->second.target != 0 && it->second.target != target)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    QueryObject& q = it->second;
    retireQuerySlots(q);
    q.target = target;
    q.ticket = 0;
    q.pending = false;
    q.count = 0;
    if (timer && !writeTimestamp(q, id)) return;
    active = id;
    // The next pass opens the first occlusion slot.
    if (occlusionTarget(target)) endRendering();
}

void WebGLVkContext::endQuery(GLenum target) {
    const bool timer = target == GL_TIME_ELAPSED_EXT && timerQueriesEnabled_;
    if (!occlusionTarget(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN && !timer) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    GLuint& active = occlusionTarget(target) ? activeOcclusionQuery_
                     : timer                 ? activeTimerQuery_
                                             : activeFeedbackQuery_;
    auto it = active != 0 ? queries_.find(active) : queries_.end();
    if (it == queries_.end() || (occlusionTarget(target) && !occlusionTarget(it->second.target))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (occlusionTarget(target)) endRendering();  // closes the open slot
    if (timer) {
        // Write the end stamp outside the render pass (transferCommands ends
        // an open pass first), matching writeTimestamp, so backends that
        // sample timestamps only at pass boundaries (Metal/MoltenVK without
        // stage counter sampling) record the end of the pass rather than its
        // start.
        QueryObject& q = it->second;
        if (q.stamps.size() == 2) {
            vkCmdWriteTimestamp(transferCommands(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                timestampSlots_.pool(q.stamps[1]), QuerySlots::index(q.stamps[1]));
            queryUsed(q, active);
        }
    }
    active = 0;
}

// The first stamp of a query: TIME_ELAPSED_EXT's begin (its end slot reset
// alongside), or TIMESTAMP_EXT's only one.
bool WebGLVkContext::writeTimestamp(QueryObject& q, GLuint id) {
    VkCommandBuffer cmd = transferCommands();
    const uint32_t count = q.target == GL_TIME_ELAPSED_EXT ? 2 : 1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t slot = 0;
        if (!allocSlot(timestampSlots_, cmd, slot)) {
            retireSlots(timestampSlots_, 0, q.stamps);
            return false;
        }
        q.stamps.push_back(slot);
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampSlots_.pool(q.stamps[0]),
                        QuerySlots::index(q.stamps[0]));
    queryUsed(q, id);
    return true;
}

void WebGLVkContext::queryCounter(GLuint id, GLenum target) {
    if (!timerQueriesEnabled_ || target != GL_TIMESTAMP_EXT) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    auto it = queries_.find(id);
    if (it == queries_.end() || id == activeOcclusionQuery_ || id == activeFeedbackQuery_ ||
        id == activeTimerQuery_ || (it->second.target != 0 && it->second.target != target)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    QueryObject& q = it->second;
    retireQuerySlots(q);
    q.target = target;
    q.ticket = 0;
    q.pending = false;
    q.count = 0;
    writeTimestamp(q, id);
}

GLint WebGLVkContext::queryCounterBits(GLenum target) {
    if (!timerQueriesEnabled_ || (target != GL_TIME_ELAPSED_EXT && target != GL_TIMESTAMP_EXT)) {
        setSyntheticError(GL_INVALID_ENUM);
        return 0;
    }
    return static_cast<GLint>(timestampBits_);
}

// Nothing bro knows of makes timings meaningless (no clock-domain changes are
// reported by Vulkan), so a disjoint operation is never signalled.
bool WebGLVkContext::gpuDisjoint() {
    if (!timerQueriesEnabled_) {
        setSyntheticError(GL_INVALID_ENUM);
        return false;
    }
    return false;
}

// The GPU's clock now, in nanoseconds: a lone timestamp submitted and waited
// for on its own ticket (the open command buffer is not submitted).
int64_t WebGLVkContext::gpuTimestamp() {
    if (!timerQueriesEnabled_) {
        setSyntheticError(GL_INVALID_ENUM);
        return 0;
    }
    std::vector<uint32_t> taken;
    const bool ok = context_.queue().submitImmediate([&](VkCommandBuffer cmd) {
        uint32_t slot = 0;
        if (!allocSlot(timestampSlots_, cmd, slot)) return;
        taken.push_back(slot);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampSlots_.pool(slot),
                            QuerySlots::index(slot));
    });
    std::vector<uint64_t> stamps;
    const bool read = ok && !taken.empty() && readSlots(context_.device(), timestampSlots_.pools, taken, true, stamps);
    retireSlots(timestampSlots_, 0, taken);
    if (!read) return 0;
    const uint64_t mask = timestampBits_ >= 64 ? ~0ull : (1ull << timestampBits_) - 1;
    return static_cast<int64_t>(static_cast<double>(stamps[0] & mask) * timestampPeriodNs_);
}

GLuint WebGLVkContext::currentQuery(GLenum target) {
    if (occlusionTarget(target)) {
        auto it = queries_.find(activeOcclusionQuery_);
        return it != queries_.end() && it->second.target == target ? activeOcclusionQuery_ : 0;
    }
    if (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) return activeFeedbackQuery_;
    if (timerQueriesEnabled_ && target == GL_TIME_ELAPSED_EXT) return activeTimerQuery_;
    if (timerQueriesEnabled_ && target == GL_TIMESTAMP_EXT) return 0;  // never active
    setSyntheticError(GL_INVALID_ENUM);
    return 0;
}

// The query's runs recorded since the last submission are in this one.
void WebGLVkContext::queriesSubmitted() {
    for (GLuint id : unsubmittedQueries_) {
        auto it = queries_.find(id);
        if (it == queries_.end() || !it->second.pending) continue;
        it->second.pending = false;
        it->second.ticket = stream_.lastTicket();
    }
    unsubmittedQueries_.clear();
    for (QuerySlots* slots : {&occlusionSlots_, &timestampSlots_})
        for (auto& [ticket, taken] : slots->retired)
            if (ticket == kUnsubmittedTicket) ticket = stream_.lastTicket();
}

void WebGLVkContext::queryUsed(QueryObject& q, GLuint id) {
    if (!q.pending) unsubmittedQueries_.push_back(id);
    q.pending = true;
}

bool WebGLVkContext::readTimerResult(const QueryObject& q, bool wait, uint64_t& ns) {
    std::vector<uint64_t> stamps;
    if (!readSlots(context_.device(), timestampSlots_.pools, q.stamps, wait, stamps) || stamps.empty())
        return false;
    const uint64_t mask = timestampBits_ >= 64 ? ~0ull : (1ull << timestampBits_) - 1;
    const uint64_t ticks = stamps.size() == 2 ? ((stamps[1] & mask) - (stamps[0] & mask)) & mask : stamps[0] & mask;
    ns = static_cast<uint64_t>(static_cast<double>(ticks) * timestampPeriodNs_);
    return true;
}

bool WebGLVkContext::getQueryParameter(GLuint id, GLenum pname, uint64_t& out) {
    auto it = queries_.find(id);
    if (it == queries_.end() || it->second.target == 0 || id == activeOcclusionQuery_ ||
        id == activeFeedbackQuery_ || id == activeTimerQuery_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    QueryObject& q = it->second;
    if (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE) {
        setSyntheticError(GL_INVALID_ENUM);
        return false;
    }
    const bool timer = q.target == GL_TIME_ELAPSED_EXT || q.target == GL_TIMESTAMP_EXT;
    if (q.pending) {
        // Recorded but not submitted: a result needs the submission.
        if (pname == GL_QUERY_RESULT_AVAILABLE) {
            out = GL_FALSE;
            return true;
        }
        flushCommands();
    }
    const bool done = q.ticket == 0 || context_.queue().isComplete(q.ticket);
    if (pname == GL_QUERY_RESULT_AVAILABLE) {
        if (!done) {
            out = GL_FALSE;
        } else if (timer) {
            // Read now and kept: the slots go back to the pool.
            uint64_t ns = 0;
            if (q.stamps.empty()) {
                out = GL_TRUE;
            } else if (readTimerResult(q, false, ns)) {
                q.count = ns;
                retireQuerySlots(q);
                out = GL_TRUE;
            } else {
                out = GL_FALSE;
            }
        } else {
            uint64_t probe = 0;
            out = readOcclusionSlots(context_.device(), occlusionSlots_.pools, q.slots, false, probe) ? GL_TRUE
                                                                                                      : GL_FALSE;
        }
        return true;
    }
    if (!done) context_.queue().wait(q.ticket);
    if (timer) {
        uint64_t ns = 0;
        if (!q.stamps.empty() && readTimerResult(q, true, ns)) q.count = ns;
        retireQuerySlots(q);
        out = q.count;
        return true;
    }
    if (!q.slots.empty()) {
        readOcclusionSlots(context_.device(), occlusionSlots_.pools, q.slots, true, q.count);
        retireQuerySlots(q);
    }
    out = occlusionTarget(q.target) ? (q.count > 0 ? 1u : 0u) : q.count;
    return true;
}

// A free slot of `slots`' type, its reset recorded in `cmd` (outside a pass).
bool WebGLVkContext::allocSlot(QuerySlots& slots, VkCommandBuffer cmd, uint32_t& slot) {
    for (size_t i = 0; i < slots.retired.size();) {
        const uint64_t ticket = slots.retired[i].first;
        if (ticket != kUnsubmittedTicket && context_.queue().isComplete(ticket)) {
            const auto& done = slots.retired[i].second;
            slots.free.insert(slots.free.end(), done.begin(), done.end());
            slots.retired.erase(slots.retired.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (slots.free.empty()) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = slots.type;
        info.queryCount = kSlotsPerPool;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (vkCreateQueryPool(context_.device(), &info, nullptr, &pool) != VK_SUCCESS) {
            setSyntheticError(GL_OUT_OF_MEMORY);
            return false;
        }
        const uint32_t base = static_cast<uint32_t>(slots.pools.size()) * kSlotsPerPool;
        slots.pools.push_back(pool);
        for (uint32_t i = kSlotsPerPool; i-- > 0;) slots.free.push_back(base + i);
    }
    slot = slots.free.back();
    slots.free.pop_back();
    vkCmdResetQueryPool(cmd, slots.pool(slot), QuerySlots::index(slot), 1);
    return true;
}

// Called by beginRendering on either side of vkCmdBeginRendering: reserve
// (and reset) a slot for the active occlusion query, then begin it.
void WebGLVkContext::prepareOcclusionSlot(VkCommandBuffer cmd) {
    openQuerySlot_ = -1;
    if (activeOcclusionQuery_ == 0 || occlusionSuspended_) return;
    uint32_t slot = 0;
    if (allocSlot(occlusionSlots_, cmd, slot)) openQuerySlot_ = static_cast<int32_t>(slot);
}

void WebGLVkContext::beginOcclusionSlot(VkCommandBuffer cmd) {
    if (openQuerySlot_ < 0) return;
    const auto slot = static_cast<uint32_t>(openQuerySlot_);
    vkCmdBeginQuery(cmd, occlusionSlots_.pool(slot), QuerySlots::index(slot), 0);
    QueryObject& q = queries_[activeOcclusionQuery_];
    q.slots.push_back(slot);
    queryUsed(q, activeOcclusionQuery_);
}

void WebGLVkContext::endOcclusionSlot(VkCommandBuffer cmd) {
    if (openQuerySlot_ < 0) return;
    const auto slot = static_cast<uint32_t>(openQuerySlot_);
    vkCmdEndQuery(cmd, occlusionSlots_.pool(slot), QuerySlots::index(slot));
    openQuerySlot_ = -1;
}

void WebGLVkContext::destroySlots(QuerySlots& slots) {
    for (VkQueryPool pool : slots.pools) vkDestroyQueryPool(context_.device(), pool, nullptr);
    slots.pools.clear();
    slots.free.clear();
    slots.retired.clear();
}

void WebGLVkContext::destroyQueries() {
    destroySlots(occlusionSlots_);
    destroySlots(timestampSlots_);
    queries_.clear();
}

} // namespace bro::webgl::vk

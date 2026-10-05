// Query objects. ANY_SAMPLES_PASSED(_CONSERVATIVE) is a Vulkan occlusion
// query: one is open for each render pass instance while the GL query is
// active (a Vulkan query cannot outlive the pass it began in, nor the command
// buffer), and the GL result is whether any of them counted a sample.
// TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN counts on the CPU, since the draws
// that capture know what they write. A result is available once the
// submission holding the query's last use has completed.

#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

namespace bro::webgl::vk {

namespace {

constexpr uint32_t kSlotsPerPool = 64;
// The ticket of slots whose last use is in the open command buffer.
constexpr uint64_t kUnsubmitted = ~0ull;

bool occlusionTarget(GLenum target) {
    return target == GL_ANY_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE;
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
    retireQuerySlots(it->second);
    queries_.erase(it);
}

// Hand a run's Vulkan slots back once the GPU is done with them: after the
// submission that last used them, or the next one if that is the open
// command buffer.
void WebGLVkContext::retireQuerySlots(QueryObject& q) {
    if (q.slots.empty()) return;
    retiredQuerySlots_.push_back({q.pending ? kUnsubmitted : q.ticket, std::move(q.slots)});
    q.slots.clear();
}

void WebGLVkContext::beginQuery(GLenum target, GLuint id) {
    if (!occlusionTarget(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    auto it = queries_.find(id);
    GLuint& active = occlusionTarget(target) ? activeOcclusionQuery_ : activeFeedbackQuery_;
    // One query per target at a time, the two occlusion targets sharing one
    // slot; a query keeps the target it was first begun with.
    if (it == queries_.end() || active != 0 || id == activeOcclusionQuery_ || id == activeFeedbackQuery_ ||
        (it->second.target != 0 && it->second.target != target)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    QueryObject& q = it->second;
    retireQuerySlots(q);
    q.target = target;
    q.ticket = 0;
    q.pending = false;
    q.count = 0;
    active = id;
    // The next pass opens the first occlusion slot.
    if (occlusionTarget(target)) endRendering();
}

void WebGLVkContext::endQuery(GLenum target) {
    if (!occlusionTarget(target) && target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    GLuint& active = occlusionTarget(target) ? activeOcclusionQuery_ : activeFeedbackQuery_;
    auto it = active != 0 ? queries_.find(active) : queries_.end();
    if (it == queries_.end() || (occlusionTarget(target) && !occlusionTarget(it->second.target))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (occlusionTarget(target)) endRendering();  // closes the open slot
    active = 0;
}

GLuint WebGLVkContext::currentQuery(GLenum target) {
    if (occlusionTarget(target)) {
        auto it = queries_.find(activeOcclusionQuery_);
        return it != queries_.end() && it->second.target == target ? activeOcclusionQuery_ : 0;
    }
    if (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) return activeFeedbackQuery_;
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
    for (auto& [ticket, slots] : retiredQuerySlots_)
        if (ticket == kUnsubmitted) ticket = stream_.lastTicket();
}

void WebGLVkContext::queryUsed(QueryObject& q, GLuint id) {
    if (!q.pending) unsubmittedQueries_.push_back(id);
    q.pending = true;
}

bool WebGLVkContext::getQueryParameter(GLuint id, GLenum pname, GLuint& out) {
    auto it = queries_.find(id);
    if (it == queries_.end() || it->second.target == 0 || id == activeOcclusionQuery_ ||
        id == activeFeedbackQuery_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    QueryObject& q = it->second;
    if (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE) {
        setSyntheticError(GL_INVALID_ENUM);
        return false;
    }
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
        out = done ? GL_TRUE : GL_FALSE;
        return true;
    }
    if (!done) context_.queue().wait(q.ticket);
    if (!q.slots.empty()) {
        for (uint32_t slot : q.slots) {
            uint64_t samples = 0;
            vkGetQueryPoolResults(context_.device(), queryPools_[slot / kSlotsPerPool], slot % kSlotsPerPool, 1,
                                  sizeof(samples), &samples, sizeof(samples), VK_QUERY_RESULT_64_BIT);
            q.count += samples;
        }
        retireQuerySlots(q);
    }
    out = occlusionTarget(q.target) ? (q.count > 0 ? 1u : 0u) : static_cast<GLuint>(q.count);
    return true;
}

// A free occlusion slot, its reset recorded in `cmd` (outside a pass).
bool WebGLVkContext::allocQuerySlot(VkCommandBuffer cmd, uint32_t& slot) {
    for (size_t i = 0; i < retiredQuerySlots_.size();) {
        const uint64_t ticket = retiredQuerySlots_[i].first;
        if (ticket != kUnsubmitted && context_.queue().isComplete(ticket)) {
            const auto& slots = retiredQuerySlots_[i].second;
            freeQuerySlots_.insert(freeQuerySlots_.end(), slots.begin(), slots.end());
            retiredQuerySlots_.erase(retiredQuerySlots_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (freeQuerySlots_.empty()) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        info.queryCount = kSlotsPerPool;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (vkCreateQueryPool(context_.device(), &info, nullptr, &pool) != VK_SUCCESS) {
            setSyntheticError(GL_OUT_OF_MEMORY);
            return false;
        }
        const uint32_t base = static_cast<uint32_t>(queryPools_.size()) * kSlotsPerPool;
        queryPools_.push_back(pool);
        for (uint32_t i = kSlotsPerPool; i-- > 0;) freeQuerySlots_.push_back(base + i);
    }
    slot = freeQuerySlots_.back();
    freeQuerySlots_.pop_back();
    vkCmdResetQueryPool(cmd, queryPools_[slot / kSlotsPerPool], slot % kSlotsPerPool, 1);
    return true;
}

// Called by beginRendering on either side of vkCmdBeginRendering: reserve
// (and reset) a slot for the active occlusion query, then begin it.
void WebGLVkContext::prepareOcclusionSlot(VkCommandBuffer cmd) {
    openQuerySlot_ = -1;
    if (activeOcclusionQuery_ == 0 || occlusionSuspended_) return;
    uint32_t slot = 0;
    if (allocQuerySlot(cmd, slot)) openQuerySlot_ = static_cast<int32_t>(slot);
}

void WebGLVkContext::beginOcclusionSlot(VkCommandBuffer cmd) {
    if (openQuerySlot_ < 0) return;
    const auto slot = static_cast<uint32_t>(openQuerySlot_);
    vkCmdBeginQuery(cmd, queryPools_[slot / kSlotsPerPool], slot % kSlotsPerPool, 0);
    QueryObject& q = queries_[activeOcclusionQuery_];
    q.slots.push_back(slot);
    queryUsed(q, activeOcclusionQuery_);
}

void WebGLVkContext::endOcclusionSlot(VkCommandBuffer cmd) {
    if (openQuerySlot_ < 0) return;
    const auto slot = static_cast<uint32_t>(openQuerySlot_);
    vkCmdEndQuery(cmd, queryPools_[slot / kSlotsPerPool], slot % kSlotsPerPool);
    openQuerySlot_ = -1;
}

void WebGLVkContext::destroyQueries() {
    for (VkQueryPool pool : queryPools_) vkDestroyQueryPool(context_.device(), pool, nullptr);
    queryPools_.clear();
    freeQuerySlots_.clear();
    retiredQuerySlots_.clear();
    queries_.clear();
}

} // namespace bro::webgl::vk

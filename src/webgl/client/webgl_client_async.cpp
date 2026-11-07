// The client's queries, syncs and extensions, and the replay's side of the
// query publications (webgl_client_async.h).
//
// A query's status is published by the replay after each batch of commands
// (WebGLReplayState::publish, which asks the backend without waiting or
// submitting: vk::WebGLVkContext::pollQuery), so QUERY_RESULT_AVAILABLE and
// an available QUERY_RESULT are answered from the slot. Only a QUERY_RESULT
// that is not available yet, or a call the backend refuses (its error is
// the backend's to raise), goes to the backend.
//
// A fence is its queue ticket, published by the replay of fenceSync; SYNC_STATUS
// and clientWaitSync read the queue's completed ticket (thread-safe) and
// never ask the backend. Every error the sync calls raise is decided here.

#include "webgl/client/webgl_client.h"
#include "webgl/client/webgl_thread.h"

#include "render/vulkan_queue.h"
#include "webgl/vulkan/webgl_vk_caps.h"
#include "webgl/vulkan/webgl_vk_context.h"

#include <algorithm>

namespace bro::webgl {

namespace {

constexpr GLenum kObjectType = 0x9112;
constexpr GLenum kSyncCondition = 0x9113;
constexpr GLenum kSyncStatus = 0x9114;
constexpr GLenum kSyncFlags = 0x9115;
constexpr GLenum kSyncFence = 0x9116;
constexpr GLenum kSyncGpuCommandsComplete = 0x9117;
constexpr GLenum kUnsignaled = 0x9118;
constexpr GLenum kSignaled = 0x9119;
constexpr GLenum kAlreadySignaled = 0x911A;
constexpr GLenum kTimeoutExpired = 0x911B;
constexpr GLenum kConditionSatisfied = 0x911C;
constexpr GLenum kWaitFailed = 0x911D;
constexpr GLbitfield kSyncFlushCommandsBit = 0x1;

uint64_t queryWord(uint32_t serial, QueryStatus status) {
    return (static_cast<uint64_t>(serial) << 8) | static_cast<uint64_t>(status);
}

// The group of targets one query can be active on at a time (the two
// occlusion targets share one); -1 for any other target.
int queryGroup(GLenum target) {
    switch (target) {
        case GL_ANY_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED_CONSERVATIVE: return 0;
        case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: return 1;
        case GL_TIME_ELAPSED_EXT: return 2;
        default: return -1;
    }
}

}  // namespace

// --- The replay's side ------------------------------------------------------

void WebGLReplayState::createQuery(GLuint id, std::shared_ptr<QuerySlot> slot) {
    QueryEntry& e = queries_[id];
    e = QueryEntry{};
    e.slot = std::move(slot);
}

void WebGLReplayState::deleteQuery(GLuint id) { queries_.erase(id); }

void WebGLReplayState::queryTouched(GLuint id) {
    auto it = queries_.find(id);
    if (it != queries_.end()) ++it->second.serial;
}

void WebGLReplayState::publish(vk::WebGLVkContext& gl) {
    for (auto& [id, e] : queries_) {
        if (e.published == QueryStatus::Available && e.publishedSerial == e.serial) continue;
        uint64_t result = 0;
        const auto status = static_cast<QueryStatus>(gl.pollQuery(id, result));
        if (status == e.published && e.publishedSerial == e.serial) continue;
        if (status == QueryStatus::Available) e.slot->result.store(result, std::memory_order_relaxed);
        e.slot->word.store(queryWord(e.serial, status), std::memory_order_release);
        e.published = status;
        e.publishedSerial = e.serial;
    }
}

// --- Queries ----------------------------------------------------------------

void WebGLClient::createQuery(GLuint id) {
    HeldQuery& q = queries_[id];
    q = HeldQuery{};
    q.slot = std::make_shared<QuerySlot>();
    encodeSimple(channel_.ring, Op::CreateQuery, rec::kCreateQuery, id, holdRef(q.slot));
}

void WebGLClient::deleteQuery(GLuint id) {
    queries_.erase(id);
    for (GLuint& active : activeQuery_)
        if (active == id) active = 0;
    encodeSimple(channel_.ring, Op::DeleteQuery, rec::kDeleteQuery, id);
}

void WebGLClient::beginQuery(GLenum target, GLuint id) {
    const int group = queryGroup(target);
    if (group >= 0 && activeQuery_[group] == 0) activeQuery_[group] = id;
    if (auto it = queries_.find(id); it != queries_.end()) {
        ++it->second.serial;
        it->second.ended = false;
    }
    encodeSimple(channel_.ring, Op::BeginQuery, rec::kBeginQuery, target, id);
}

void WebGLClient::endQuery(GLenum target) {
    const int group = queryGroup(target);
    GLuint id = 0;
    if (group >= 0) std::swap(id, activeQuery_[group]);
    if (auto it = queries_.find(id); it != queries_.end()) {
        ++it->second.serial;
        it->second.ended = true;
    }
    encodeSimple(channel_.ring, Op::EndQuery, rec::kEndQuery, target, id);
}

void WebGLClient::queryCounter(GLuint id, GLenum target) {
    if (auto it = queries_.find(id); it != queries_.end()) {
        ++it->second.serial;
        it->second.ended = target == GL_TIMESTAMP_EXT && std::find(std::begin(activeQuery_), std::end(activeQuery_), id) ==
                                                            std::end(activeQuery_);
    }
    encodeSimple(channel_.ring, Op::QueryCounter, rec::kQueryCounter, id, target);
}

bool WebGLClient::queryStatus(GLuint id, QueryStatus& status, uint64_t& result) {
    auto it = id != 0 ? queries_.find(id) : queries_.end();
    if (it == queries_.end()) return false;
    const HeldQuery& q = it->second;
    QuerySlot& slot = *q.slot;
    uint64_t word = slot.word.load(std::memory_order_acquire);
    if ((word >> 8) != q.serial) {
        if (q.ended) {
            // Ended, not replayed yet: its result is not on the host, and
            // the WebGL thread will publish it (no wait; an app polls this
            // every frame).
            word = (static_cast<uint64_t>(q.serial) << 8) | static_cast<uint64_t>(QueryStatus::Pending);
        } else {
            // A begin not replayed yet (the backend says whether it took):
            // wait for it, once.
            waitFor([&] { return (slot.word.load(std::memory_order_acquire) >> 8) == q.serial; });
            word = slot.word.load(std::memory_order_acquire);
        }
    }
    status = static_cast<QueryStatus>(word & 0xff);
    if (status == QueryStatus::Available) {
        result = slot.result.load(std::memory_order_relaxed);
    } else if (status == QueryStatus::Pending) {
        // Ask the WebGL thread to look again, even with nothing to replay,
        // so a later read finds the result once the GPU has it.
        channel_.pollRequested.store(true, std::memory_order_relaxed);
        thread_->wake();
    }
    return true;
}

// --- Syncs ------------------------------------------------------------------

bool WebGLClient::fenceSync(GLuint id, GLenum condition, GLbitfield flags) {
    if (condition != kSyncGpuCommandsComplete) {
        syntheticError(GL_INVALID_ENUM);
        return false;
    }
    if (flags != 0) {
        syntheticError(GL_INVALID_VALUE);
        return false;
    }
    auto slot = std::make_shared<FenceSlot>();
    fences_[id] = slot;
    encodeSimple(channel_.ring, Op::FenceSync, rec::kFenceSync, id, condition, flags, holdRef(std::move(slot)));
    return true;
}

void WebGLClient::dropFence(GLuint id) {
    fences_.erase(id);
    deleteSync(id);
}

// The fence's ticket, once its fenceSync has replayed (waiting for that once:
// the WebGL thread replaying what is before it, never a wait on the GPU).
uint64_t WebGLClient::fenceTicket(FenceSlot& slot) {
    waitFor([&] { return slot.ticketPlus1.load(std::memory_order_acquire) != 0; });
    return slot.ticketPlus1.load(std::memory_order_acquire) - 1;
}

GLenum WebGLClient::clientWaitSync(GLuint id, GLbitfield flags, double timeoutNs) {
    auto it = id != 0 ? fences_.find(id) : fences_.end();
    if (it == fences_.end()) {
        syntheticError(GL_INVALID_OPERATION);  // deleted
        return kWaitFailed;
    }
    if ((flags & ~kSyncFlushCommandsBit) != 0 || timeoutNs < 0) {
        syntheticError(GL_INVALID_VALUE);
        return kWaitFailed;
    }
    // WebGL 2.0 caps client waits at MAX_CLIENT_WAIT_TIMEOUT_WEBGL: a longer
    // one would stall the script thread.
    if (timeoutNs > vk::WebGLVkContext::kMaxClientWaitTimeoutNs) {
        syntheticError(GL_INVALID_OPERATION);
        return kWaitFailed;
    }
    // The fence's own submission went at fenceSync: SYNC_FLUSH_COMMANDS_BIT
    // has nothing left to flush.
    const uint64_t ticket = fenceTicket(*it->second);
    if (queue_.isComplete(ticket)) return kAlreadySignaled;
    if (timeoutNs == 0) return kTimeoutExpired;
    return queue_.wait(ticket, static_cast<uint64_t>(timeoutNs)) ? kConditionSatisfied : kTimeoutExpired;
}

// The queue already orders this context's later work after the fence, so a
// server wait has nothing to wait for once its arguments are valid.
void WebGLClient::waitSync(GLuint id, GLbitfield flags, double timeoutNs) {
    if (flags != 0 || timeoutNs != -1.0)
        syntheticError(GL_INVALID_VALUE);
    else if (!isFence(id))
        syntheticError(GL_INVALID_OPERATION);
}

bool WebGLClient::getSyncParameter(GLuint id, GLenum pname, GLint& out) {
    auto it = id != 0 ? fences_.find(id) : fences_.end();
    if (it == fences_.end()) {
        syntheticError(GL_INVALID_OPERATION);
        return false;
    }
    switch (pname) {
        case kObjectType: out = kSyncFence; return true;
        case kSyncCondition: out = kSyncGpuCommandsComplete; return true;
        case kSyncFlags: out = 0; return true;
        case kSyncStatus:
            out = queue_.isComplete(fenceTicket(*it->second)) ? kSignaled : kUnsignaled;
            return true;
        default:
            syntheticError(GL_INVALID_ENUM);
            return false;
    }
}

// --- Extensions -------------------------------------------------------------

bool WebGLClient::getExtension(const std::string& name) {
    if (!caps_.supports(name)) return false;
    if (!extensionEnabled(name)) extensions_.push_back(name);
    enableExtension(name);
    return true;
}

bool WebGLClient::extensionEnabled(std::string_view name) const {
    return std::find(extensions_.begin(), extensions_.end(), name) != extensions_.end();
}

std::vector<GLint> WebGLClient::compressedTextureFormats() const {
    // In the device's extension order, whatever order they were enabled in.
    std::vector<GLint> formats;
    for (const vk::WebGLCaps::CompressedExtension& ext : caps_.compressedExtensions) {
        if (!extensionEnabled(ext.name)) continue;
        for (GLint f : ext.formats)
            if (std::find(formats.begin(), formats.end(), f) == formats.end()) formats.push_back(f);
    }
    return formats;
}

}  // namespace bro::webgl

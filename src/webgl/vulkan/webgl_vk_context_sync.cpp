// Sync objects (WebGL2 fenceSync and friends). A fence is the queue ticket of
// the submission that holds everything recorded before it: fenceSync submits
// the open command buffer, and the fence is signaled once that ticket has
// completed. Client waits wait on that one ticket, never on the device.

#include "webgl/vulkan/webgl_vk_context.h"

namespace bro::webgl::vk {

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

} // namespace

GLuint WebGLVkContext::fenceSync(GLenum condition, GLbitfield flags) {
    if (condition != kSyncGpuCommandsComplete) {
        setSyntheticError(GL_INVALID_ENUM);
        return 0;
    }
    if (flags != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return 0;
    }
    flushCommands();
    const GLuint id = nextObjectId_++;
    syncs_[id] = stream_.lastTicket();
    return id;
}

void WebGLVkContext::deleteSync(GLuint id) {
    syncs_.erase(id);
}

GLboolean WebGLVkContext::isSync(GLuint id) const {
    return id != 0 && syncs_.count(id) ? GL_TRUE : GL_FALSE;
}

GLenum WebGLVkContext::clientWaitSync(GLuint id, GLbitfield flags, double timeoutNs) {
    auto it = syncs_.find(id);
    if (it == syncs_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);  // deleted
        return kWaitFailed;
    }
    if ((flags & ~kSyncFlushCommandsBit) != 0 || timeoutNs < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return kWaitFailed;
    }
    // WebGL 2.0 caps client waits at MAX_CLIENT_WAIT_TIMEOUT_WEBGL: a longer one would stall
    // the script thread.
    if (timeoutNs > kMaxClientWaitTimeoutNs) {
        setSyntheticError(GL_INVALID_OPERATION);
        return kWaitFailed;
    }
    const uint64_t ticket = it->second;
    if (context_.queue().isComplete(ticket)) return kAlreadySignaled;
    if (timeoutNs == 0) return kTimeoutExpired;
    return context_.queue().wait(ticket, static_cast<uint64_t>(timeoutNs)) ? kConditionSatisfied
                                                                          : kTimeoutExpired;
}

// The queue already orders this context's later work after the fence, so a
// server wait has nothing to wait for once its arguments are valid.
void WebGLVkContext::waitSync(GLuint id, GLbitfield flags, double timeoutNs) {
    if (flags != 0 || timeoutNs != -1.0) setSyntheticError(GL_INVALID_VALUE);
    else if (syncs_.find(id) == syncs_.end()) setSyntheticError(GL_INVALID_OPERATION);
}

bool WebGLVkContext::getSyncParameter(GLuint id, GLenum pname, GLint& out) {
    auto it = syncs_.find(id);
    if (it == syncs_.end()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    switch (pname) {
        case kObjectType: out = kSyncFence; return true;
        case kSyncCondition: out = kSyncGpuCommandsComplete; return true;
        case kSyncFlags: out = 0; return true;
        case kSyncStatus:
            out = context_.queue().isComplete(it->second) ? kSignaled : kUnsignaled;
            return true;
        default:
            setSyntheticError(GL_INVALID_ENUM);
            return false;
    }
}

} // namespace bro::webgl::vk

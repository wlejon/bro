#include "webgl/client/webgl_blob.h"

#include <algorithm>

namespace bro::webgl {

WebGLBlobPool::~WebGLBlobPool() {
    collect();
    for (WebGLBlob* b : kept_) delete b;
}

// Move what the replay gave back into kept_, freeing past kKeepBytes (the
// newest returns are kept: they are what the next frame asks for again).
void WebGLBlobPool::collect() {
    WebGLBlob* b = returned_.exchange(nullptr, std::memory_order_acquire);
    while (b) {
        WebGLBlob* next = b->next;
        b->next = nullptr;
        const size_t cap = b->bytes.capacity();
        if (keptBytes_ + cap <= kKeepBytes) {
            kept_.push_back(b);
            keptBytes_ += cap;
        } else {
            delete b;
        }
        b = next;
    }
}

WebGLBlob* WebGLBlobPool::take(size_t bytes) {
    if (returned_.load(std::memory_order_relaxed)) collect();
    // The smallest kept blob the bytes fit, if it is not much larger (a
    // small upload must not take the blob a large one comes back for).
    size_t best = kept_.size();
    for (size_t i = 0; i < kept_.size(); ++i) {
        const size_t cap = kept_[i]->bytes.capacity();
        if (cap < bytes || cap > bytes * 2 + 65536) continue;
        if (best == kept_.size() || cap < kept_[best]->bytes.capacity()) best = i;
    }
    WebGLBlob* b;
    if (best < kept_.size()) {
        b = kept_[best];
        keptBytes_ -= b->bytes.capacity();
        kept_[best] = kept_.back();
        kept_.pop_back();
    } else {
        b = new WebGLBlob;
    }
    b->bytes.resize(bytes);
    return b;
}

}  // namespace bro::webgl

#pragma once
#include "dom/document.h"

#include <cstdint>

namespace bro::dom {

/// Weak, generation-checked reference to a node — the safe way to cache a
/// DOM pointer across frames or events. Replaces the raw-pointer +
/// reap/scrub pattern: instead of hunting down every cached pointer when a
/// node dies, holders resolve lazily and get nullptr once the node (or its
/// whole document) is gone.
///
/// get() never dereferences a dead pointer: it first checks the document
/// live-set, then does a pointer-value lookup in the document's ownership
/// maps, and only then compares the node's never-recycled id. A node queued
/// for deferred free (freeNode'd but not yet drained) still resolves — its
/// memory is alive and in-flight traversals may still reference it — which
/// matches the liveness rule the old reapDeadInputPointers() used.
///
/// Main-thread only, like all DOM mutation. Not for cross-thread use: other
/// threads must keep working from snapshots taken under phase discipline.
template <typename T>
class NodeHandle {
public:
    NodeHandle() = default;
    NodeHandle(Document* doc, T* node) { assign(doc, node); }

    void assign(Document* doc, T* node) {
        doc_ = node ? doc : nullptr;
        ptr_ = node;
        id_  = node ? node->nodeId() : 0;
    }

    void reset() {
        doc_ = nullptr;
        ptr_ = nullptr;
        id_  = 0;
    }

    /// The node if it is still alive, else nullptr. The static_cast is sound
    /// because a matching id proves this is the same object assign() saw.
    T* get() const {
        if (!ptr_ || !Document::isLiveDocument(doc_)) return nullptr;
        return static_cast<T*>(doc_->resolveNode(ptr_, id_));
    }

    explicit operator bool() const { return get() != nullptr; }

    /// True if the handle was assigned a node, whether or not it is still
    /// alive. `held() && !get()` distinguishes "target died" from "never set"
    /// — e.g. pointer lock must release when its element is freed.
    bool held() const { return ptr_ != nullptr; }

private:
    Document* doc_ = nullptr;
    T* ptr_ = nullptr;
    uint32_t id_ = 0;
};

using ElementHandle  = NodeHandle<Element>;
using TextNodeHandle = NodeHandle<TextNode>;

} // namespace bro::dom

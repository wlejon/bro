#pragma once

#include "render/draw_command.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <include/core/SkRefCnt.h>
#include <include/core/SkTextBlob.h>

namespace bro::render {

// A flat list of DrawCommands plus an arena for variable-length payloads
// referenced by (offset, len). Built on the main thread by RecordingRenderer
// after layout, consumed read-only on the raster thread by CommandReplayer.
//
// Two CommandBuffers ping-pong between threads: the back buffer is being
// recorded into while the front buffer is being replayed. Buffers are
// reusable across frames (clear()), they are not freed.
class CommandBuffer {
public:
    void clear() {
        cmds_.clear();
        arena_.clear();
        blobs_.clear();
        pixels_.clear();
        spans_.clear();
        ownerId_ = 0;
        hasBackdrop_ = false;
        hasDither_ = false;
        hasLayer_ = false;
        boundsState_ = 0;
    }

    // The element a span was recorded for (its node id; 0 for a buffer that
    // is not a span): two recordings of one element are diffed against each
    // other for the damage between them (render/damage.h).
    void setOwnerId(uint64_t id) { ownerId_ = id; }
    uint64_t ownerId() const { return ownerId_; }
    // A backdrop filter was recorded here or in a span it holds: what it
    // shows depends on pixels anywhere under it, so no damage rect can
    // bound a change (render/damage.h).
    void noteBackdrop() { hasBackdrop_ = true; }
    bool hasBackdrop() const { return hasBackdrop_; }
    // A gradient was recorded here or in a span it holds (dithered: its
    // pixels depend on where in its device they land), and a layer (whose
    // device starts where the clip it opens under does).
    void noteDither() { hasDither_ = true; }
    bool hasDither() const { return hasDither_; }
    void noteLayer() { hasLayer_ = true; }
    bool hasLayer() const { return hasLayer_; }

    // render/damage.cpp's memo of this buffer's bounds in its own space
    // (main thread only, after the buffer is recorded): 0 not computed,
    // 1 bounded (boundsLTRB), 2 unbounded.
    mutable int boundsState_ = 0;
    mutable float boundsLTRB_[4] = {0, 0, 0, 0};

    void reserve(size_t cmdHint, size_t arenaHint) {
        cmds_.reserve(cmdHint);
        arena_.reserve(arenaHint);
    }

    // Append a command. Returns nothing; payload is moved into the variant.
    template <typename T>
    void append(T&& cmd) {
        cmds_.emplace_back(std::forward<T>(cmd));
    }

    // Copy `len` raw bytes into the arena. Returns the offset (in bytes) at
    // which they were placed. Caller stores (offset, len) inside the command.
    uint32_t pushBytes(const void* data, size_t len) {
        const uint32_t offset = static_cast<uint32_t>(arena_.size());
        if (len > 0) {
            const auto* p = reinterpret_cast<const std::byte*>(data);
            arena_.insert(arena_.end(), p, p + len);
        }
        return offset;
    }

    // Push a string view. Returns (offset, len).
    std::pair<uint32_t, uint32_t> pushString(std::string_view s) {
        return {pushBytes(s.data(), s.size()), static_cast<uint32_t>(s.size())};
    }

    // Push a span of trivially-copyable values. Returns (offset, count).
    template <typename T>
    std::pair<uint32_t, uint32_t> pushSpan(std::span<const T> items) {
        const uint32_t offset = pushBytes(items.data(), items.size_bytes());
        return {offset, static_cast<uint32_t>(items.size())};
    }

    // Shaped text is recorded as an SkTextBlob rather than as a string, so the
    // raster thread replays glyphs instead of re-shaping (see
    // RecordingRenderer::drawTextEx). Blobs are immutable and refcounted, so
    // holding them here is safe across the record/replay thread boundary; the
    // arena can't store them because it is raw POD bytes. The refs live until
    // the buffer is recycled for the next recording.
    uint32_t pushTextBlob(sk_sp<SkTextBlob> blob) {
        blobs_.push_back(std::move(blob));
        return static_cast<uint32_t>(blobs_.size() - 1);
    }

    static constexpr uint32_t kNoTextBlob = 0xFFFFFFFFu;

    // Shared pixels (Renderer::drawSharedPixels) are held by reference, like
    // text blobs: the entry keeps its owner alive until the buffer is cleared,
    // so a replay on another thread reads them however long it takes.
    uint32_t pushSharedPixels(const SharedPixels& px) {
        pixels_.push_back(px);
        return static_cast<uint32_t>(pixels_.size() - 1);
    }

    // A span recorded into a buffer of its own (Cmd_ReplaySpan), held by
    // reference like a text blob. A span is never written again once
    // recorded, so this buffer and the cache that made it can share it, and
    // a replay on another thread reads it however long it takes.
    uint32_t pushSpanBuffer(std::shared_ptr<const CommandBuffer> span) {
        spans_.push_back(std::move(span));
        return static_cast<uint32_t>(spans_.size() - 1);
    }

    // ---- read-side accessors (replayer) ----

    const CommandBuffer* spanAt(uint32_t index) const {
        return index < spans_.size() ? spans_[index].get() : nullptr;
    }

    const SkTextBlob* textBlobAt(uint32_t index) const {
        return index < blobs_.size() ? blobs_[index].get() : nullptr;
    }
    const SharedPixels* sharedPixelsAt(uint32_t index) const {
        return index < pixels_.size() ? &pixels_[index] : nullptr;
    }

    const std::vector<DrawCommand>& commands() const { return cmds_; }

    std::string_view stringAt(uint32_t offset, uint32_t len) const {
        return {reinterpret_cast<const char*>(arena_.data() + offset), len};
    }

    template <typename T>
    std::span<const T> spanAt(uint32_t offset, uint32_t count) const {
        return {reinterpret_cast<const T*>(arena_.data() + offset), count};
    }

    const std::byte* bytesAt(uint32_t offset) const {
        return arena_.data() + offset;
    }

    size_t commandCount() const { return cmds_.size(); }
    size_t arenaSize()    const { return arena_.size(); }

private:
    std::vector<DrawCommand>       cmds_;
    std::vector<std::byte>         arena_;
    std::vector<sk_sp<SkTextBlob>> blobs_;
    std::vector<SharedPixels>      pixels_;
    std::vector<std::shared_ptr<const CommandBuffer>> spans_;
    uint64_t ownerId_ = 0;
    bool hasBackdrop_ = false;
    bool hasDither_ = false;
    bool hasLayer_ = false;
};

} // namespace bro::render

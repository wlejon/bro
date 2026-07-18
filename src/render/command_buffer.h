#pragma once

#include "render/draw_command.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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
    }

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

    // ---- read-side accessors (replayer) ----

    const SkTextBlob* textBlobAt(uint32_t index) const {
        return index < blobs_.size() ? blobs_[index].get() : nullptr;
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
};

} // namespace bro::render

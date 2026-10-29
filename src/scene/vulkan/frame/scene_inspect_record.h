#pragma once

// Inspection as a recording (scene_view_inspect.h): a flight keeps the
// renders it wants compared, each its depth, its luma and its camera,
// written to one file by a thread of its own while the flight goes on; the
// comparison runs over the file afterwards, as many times and with as many
// questions as wanted, the frames shared out over threads. The comparison
// is the inspector's own, frame by frame against the one kept before.
//
// The file: "BROINSP2", u32 its scale; then each frame u32 width, u32
// height, the camera (InspectCamera: three matrices' 16 floats each, the
// eye as doubles), the depth (reversed-Z floats) and the
// luma (bytes), each width x height, top-down. A frame is the render cut
// `scale` times each way: the depth of each block's middle pixel, the luma
// its mean.

#include "scene/vulkan/frame/scene_view_inspect.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bro::scene::vk {

class InspectRecorder {
public:
    InspectRecorder() = default;
    InspectRecorder(const InspectRecorder&) = delete;
    InspectRecorder& operator=(const InspectRecorder&) = delete;
    ~InspectRecorder() { close(); }

    /// Start a recording at `path` (replaced), renders cut `scale` (1 .. 8)
    /// times each way; false, with why in `err`, when the file cannot be made.
    bool open(const std::string& path, int scale, std::string& err);
    bool isOpen() const { return file_ != nullptr; }
    /// Keep a render (`depth` reversed-Z, `rgba` top-down, both `width` x
    /// `height`): cut down here, written by the writer. Waits only while
    /// the writer is kQueued frames behind.
    void add(const InspectCamera& cam, uint32_t width, uint32_t height, const float* depth, const uint8_t* rgba);
    /// Write what is queued and close the file.
    void close();
    uint64_t frames() const { return frames_; }

    static constexpr size_t kQueued = 8;

private:
    void writer();

    std::FILE* file_ = nullptr;
    int scale_ = 1;
    uint64_t frames_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    bool closing_ = false;
};

/// Every frame of the recording at `path` compared with the one before it
/// (the first with none): `stats` frame after frame, ViewInspector::kStatCount
/// each. With `image` not None, `images` gets the frames `imageFrames` name
/// (RGBA, the recording's size), in that order. False, with why in `err`,
/// when the file is not a recording.
bool inspectRecording(const std::string& path, ViewInspector::Image image, const std::vector<uint32_t>& imageFrames,
                      std::vector<double>& stats, std::vector<std::vector<uint8_t>>& images, uint32_t& width,
                      uint32_t& height, std::string& err);

}  // namespace bro::scene::vk

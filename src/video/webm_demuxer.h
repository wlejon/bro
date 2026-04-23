#pragma once

#include "video/media_source.h"
#include <memory>
#include <string>

namespace mkvparser { class MkvReader; class Segment; class Cluster; class BlockEntry; }

namespace bro::video {

// WebM file demuxer backed by libwebm's mkvparser. Emits MediaPackets in
// container order. Supports seeking via the segment's cue points.
//
// Single-threaded by contract: construct and drive from one thread.
class WebMDemuxer final : public MediaSource {
public:
    WebMDemuxer();
    ~WebMDemuxer() override;

    // Opens the file and parses the segment header + all tracks. Returns
    // false if the file is missing, not WebM, or uses an unsupported codec.
    bool open(const std::string& path);

    const std::vector<TrackInfo>& tracks() const override { return tracks_; }
    bool readPacket(MediaPacket& out) override;
    bool seekTo(TimeNs pts) override;

    const std::string& lastError() const { return lastError_; }

private:
    bool advanceToNextBlock();

    std::unique_ptr<mkvparser::MkvReader> reader_;
    std::unique_ptr<mkvparser::Segment> segment_;  // owns cluster data
    std::vector<TrackInfo> tracks_;

    // Cursor into the segment's linked list of clusters/blocks.
    const mkvparser::Cluster* cluster_ = nullptr;
    const mkvparser::BlockEntry* blockEntry_ = nullptr;
    int blockFrameIndex_ = 0;  // index within the current Block's frames

    std::string lastError_;
};

} // namespace bro::video

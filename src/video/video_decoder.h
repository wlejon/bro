#pragma once

#include "video/media_packet.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace bro::video {

// Decoded video frame in I420 (YUV 4:2:0) planar layout. Y/U/V planes may
// have independent strides; buffers are shared so frames can be enqueued
// across threads without copying.
//
// I420 is the native output of libvpx (and dav1d). Converting to RGB on
// the CPU is wasted work — the render path will sample YUV in a shader.
struct VideoFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    TimeNs pts = 0;

    // Plane pointers are stable for the lifetime of `storage`.
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;

    // Owning buffer. libvpx owns its own output buffer internally, so in
    // practice this stays empty when frames are consumed synchronously
    // before the next decode call. Callers that hand frames to another
    // thread must copy into `storage` and repoint the plane pointers.
    std::shared_ptr<std::vector<uint8_t>> storage;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    // Push one compressed packet. Returns false on hard decode error;
    // soft errors (missing reference frames after packet loss) should
    // be swallowed internally and signaled via needsKeyframe().
    virtual bool decode(const MediaPacket& pkt) = 0;

    // Pull the next decoded frame. May be called multiple times after a
    // single decode() if the codec emits more than one frame per packet.
    virtual bool nextFrame(VideoFrame& out) = 0;

    // Drop all buffered state after the source has jumped. Codecs that
    // reorder frames (H.264/HEVC/AV1 with B-frames) hold several frames in
    // flight, and replaying those across a seek emits pictures from the old
    // position with timestamps from the new one. VP8/VP9 don't reorder, which
    // is why the built-in path never needed this.
    virtual void flush() {}

    // No more packets are coming. Reordering codecs hold finished pictures
    // back — an HEVC decoder can be sitting on sixteen of them — and without
    // being told the stream has ended they wait forever for a packet that
    // decides the order. After this, nextFrame() keeps returning true until
    // that buffer is empty. Nothing else changes: decode() after a flush()
    // starts a fresh stream, which is what a seek away from the end does.
    //
    // Cost of not calling it: the tail of every reordered file is invisible.
    // VP8/VP9 don't reorder, which is why the built-in path never needed it.
    virtual void drain() {}

    // Signal to the encoder peer that a keyframe is needed to recover
    // from unreferenced frames after a loss burst.
    virtual bool needsKeyframe() const { return false; }
};

// VP8/VP9 decoder via libvpx. Picks the codec iface from the first packet
// fed in (or the codec passed at open()). lowLatency=true configures the
// decoder to skip reorder buffering — required for calling, fine for file.
std::unique_ptr<VideoDecoder> createVpxDecoder(Codec codec, bool lowLatency);

} // namespace bro::video

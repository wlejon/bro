// Smoke test for the media backend registry (video/media_backend.h).
//
// The registry only earns its keep if a SECOND backend actually takes
// precedence and a partial one falls through — neither of which the built-in
// WebM backend can demonstrate on its own. So this registers fakes and checks
// the selection rules directly. Not linked into bro.

#include "video/media_backend.h"
#include "video/video_pipeline.h"

#include <cstdio>
#include <string>

using namespace bro::video;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

// A source that reports whatever tracks it was handed and no packets.
class FakeSource : public MediaSource {
public:
    explicit FakeSource(std::vector<TrackInfo> tracks) : tracks_(std::move(tracks)) {}
    const std::vector<TrackInfo>& tracks() const override { return tracks_; }
    bool readPacket(MediaPacket&) override { return false; }

private:
    std::vector<TrackInfo> tracks_;
};

TrackInfo videoTrack(Codec codec) {
    TrackInfo t;
    t.id = 1;
    t.kind = TrackKind::Video;
    t.codec = codec;
    t.width = 320;
    t.height = 240;
    t.durationNs = 1000000000;
    return t;
}

TrackInfo audioTrack(Codec codec) {
    TrackInfo t;
    t.id = 2;
    t.kind = TrackKind::Audio;
    t.codec = codec;
    t.sampleRate = 48000;
    t.channels = 2;
    return t;
}

class NullVideoDecoder : public VideoDecoder {
public:
    bool decode(const MediaPacket&) override { return true; }
    bool nextFrame(VideoFrame&) override { return false; }
};

class NullAudioDecoder : public AudioDecoder {
public:
    bool decode(const MediaPacket&, AudioFrame&) override { return false; }
};

// ── a stream that reorders ────────────────────────────────────────────────
//
// H.264 and HEVC hand pictures back several frames after the packet that
// carried them, and hold that many in flight until the stream ends. Neither
// the built-in WebM path nor the fakes above reorder at all, so nothing here
// could show what happens at the end of such a file: the decoder sits on its
// buffer waiting for a packet, the demuxer has none left, and the tail of the
// file is never seen. These two model exactly that.

constexpr int kFrames = 30;
constexpr int kHold = 16;                       // an HEVC-sized reorder buffer
constexpr TimeNs kFrameNs = 33333333;

class ReorderSource : public MediaSource {
public:
    ReorderSource() {
        TrackInfo t = videoTrack(Codec::H265);
        t.durationNs = kFrames * kFrameNs;
        tracks_.push_back(t);
    }
    const std::vector<TrackInfo>& tracks() const override { return tracks_; }
    bool readPacket(MediaPacket& out) override {
        if (next_ >= kFrames) return false;
        out.trackId = 1;
        out.pts = next_ * kFrameNs;
        out.keyframe = next_ == 0;
        out.data = std::make_shared<std::vector<uint8_t>>(1, 0);
        ++next_;
        return true;
    }
    bool seekTo(TimeNs pts) override {
        next_ = static_cast<int>(pts / kFrameNs);
        if (next_ < 0) next_ = 0;
        return true;
    }

private:
    std::vector<TrackInfo> tracks_;
    int next_ = 0;
};

class ReorderDecoder : public VideoDecoder {
public:
    bool decode(const MediaPacket& pkt) override {
        pending_.push_back(pkt.pts);
        return true;
    }
    bool nextFrame(VideoFrame& out) override {
        // Hand nothing back until the buffer is full — or until told the
        // stream has ended, which is the whole point.
        if (pending_.empty()) return false;
        if (!draining_ && pending_.size() <= kHold) return false;
        out.width = 16;
        out.height = 16;
        out.pts = pending_.front();
        pending_.erase(pending_.begin());
        out.y = plane_;
        out.u = plane_ + 256;
        out.v = plane_ + 256 + 64;
        out.strideY = 16;
        out.strideU = 8;
        out.strideV = 8;
        return true;
    }
    void drain() override { draining_ = true; }
    void flush() override { pending_.clear(); draining_ = false; }

private:
    std::vector<TimeNs> pending_;
    bool draining_ = false;
    uint8_t plane_[256 + 64 + 64] = {};
};

} // namespace

int main() {
    std::printf("media backend registry\n");

    // The built-in is present before anything registers, and nothing else is.
    check(mediaBackends().size() == 1, "built-in webm backend is registered by default");
    check(!mediaBackends().empty() && mediaBackends()[0].name == "webm",
          "the built-in is the webm backend");

    // A backend that recognises nothing must not disturb the order or the
    // outcome.
    MediaBackend declines;
    declines.name = "declines-everything";
    declines.priority = 50;
    declines.open = [](const std::string&) -> std::unique_ptr<MediaSource> { return nullptr; };
    registerMediaBackend(declines);
    check(mediaBackends().size() == 2, "second backend registered");
    check(mediaBackends()[0].name == "declines-everything",
          "higher priority sorts first");
    check(mediaBackends()[1].name == "webm", "built-in sorts after");

    // A backend that opens the container but cannot decode its video track
    // must NOT claim the file — the pipeline has to keep looking. This is the
    // case that would otherwise turn "some other backend could have played
    // this" into a hard failure.
    MediaBackend videoless;
    videoless.name = "opens-but-cannot-decode";
    videoless.priority = 40;
    videoless.open = [](const std::string&) -> std::unique_ptr<MediaSource> {
        return std::make_unique<FakeSource>(std::vector<TrackInfo>{videoTrack(Codec::H264)});
    };
    videoless.makeVideoDecoder = [](const TrackInfo&) -> std::unique_ptr<VideoDecoder> {
        return nullptr;      // recognises the container, not the codec
    };
    registerMediaBackend(videoless);

    {
        VideoPipeline p;
        check(!p.open("no-such-file.xyz"),
              "a backend that cannot decode the video track does not claim the file");
    }

    // A fully working backend at the highest priority takes over, including
    // for a codec the engine itself has never heard of.
    MediaBackend fake;
    fake.name = "fake";
    fake.priority = 100;
    fake.open = [](const std::string& path) -> std::unique_ptr<MediaSource> {
        if (path != "fake://clip") return nullptr;
        return std::make_unique<FakeSource>(
            std::vector<TrackInfo>{videoTrack(Codec::H264), audioTrack(Codec::AAC)});
    };
    fake.makeVideoDecoder = [](const TrackInfo& t) -> std::unique_ptr<VideoDecoder> {
        return t.codec == Codec::H264 ? std::make_unique<NullVideoDecoder>() : nullptr;
    };
    fake.makeAudioDecoder = [](const TrackInfo& t) -> std::unique_ptr<AudioDecoder> {
        return t.codec == Codec::AAC ? std::make_unique<NullAudioDecoder>() : nullptr;
    };
    registerMediaBackend(fake);
    check(mediaBackends()[0].name == "fake", "highest priority is tried first");

    {
        VideoPipeline p;
        check(p.open("fake://clip"), "a registered backend opens a non-WebM source");
        check(p.frameWidth() == 320 && p.frameHeight() == 240,
              "track dimensions reach the pipeline");
        check(p.durationNs() == 1000000000, "duration reaches the pipeline");
        check(p.audioDecoder() != nullptr,
              "the audio decoder comes from the same backend");
    }

    // The end of a reordering stream. Without draining the decoder at end of
    // stream, everything still inside it — a whole DPB, sixteen pictures for
    // HEVC — is thrown away, and playback stops a second short of the end on
    // every H.264/HEVC file there is.
    MediaBackend reorder;
    reorder.name = "reorder";
    reorder.priority = 120;
    reorder.open = [](const std::string& path) -> std::unique_ptr<MediaSource> {
        return path == "reorder://clip" ? std::make_unique<ReorderSource>() : nullptr;
    };
    reorder.makeVideoDecoder = [](const TrackInfo&) -> std::unique_ptr<VideoDecoder> {
        return std::make_unique<ReorderDecoder>();
    };
    registerMediaBackend(reorder);

    {
        const TimeNs last = (kFrames - 1) * kFrameNs;
        VideoPipeline p;
        check(p.open("reorder://clip"), "a reordering source opens");

        // `settleAt` and not `advanceTo`, throughout: the landing rule being
        // asserted is the same one — the last picture at or before the instant
        // — but the decode is on a worker thread now, so `advanceTo` answers
        // with whatever has been handed over by the time it is called. That is
        // the right answer for a screen and no answer at all for a test, which
        // would be asserting how far a thread happened to have got. Nothing
        // about the contract below is weakened by waiting for it.

        // Mid-file the buffer hides nothing: pictures come out kHold packets
        // late, which the demuxer stays ahead of.
        p.settleAt(10 * kFrameNs);
        check(p.currentPts() == 10 * kFrameNs, "mid-stream lands on the right picture");
        check(!p.isEnded(), "and does not think the file is over");

        // The tail is the part that only a drain can produce.
        p.settleAt(last);
        check(p.currentPts() == last, "the last picture of a reordering file is shown");
        check(p.isEnded(), "and only then is the file ended");

        // Asking past the end stays on the last picture rather than losing it.
        p.settleAt(last + 5 * kFrameNs);
        check(p.currentPts() == last, "advancing past the end holds the last picture");

        // Seeking back after the drain has to work: a decoder told the stream
        // ended will refuse packets until it is flushed.
        p.seekTo(4 * kFrameNs);
        check(p.currentPts() == 4 * kFrameNs, "seeking back after the end still decodes");
        check(!p.isEnded(), "and the file is no longer ended");
    }

    // A path no backend claims still fails cleanly rather than half-opening.
    {
        VideoPipeline p;
        check(!p.open("unclaimed://nothing"), "an unclaimed path fails to open");
        check(p.audioDecoder() == nullptr, "a failed open leaves no audio decoder behind");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}

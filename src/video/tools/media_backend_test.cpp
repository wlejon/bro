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

    // A path no backend claims still fails cleanly rather than half-opening.
    {
        VideoPipeline p;
        check(!p.open("unclaimed://nothing"), "an unclaimed path fails to open");
        check(p.audioDecoder() == nullptr, "a failed open leaves no audio decoder behind");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}

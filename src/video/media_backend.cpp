#include "video/media_backend.h"

#include "video/webm_demuxer.h"

#include <algorithm>

namespace bro::video {

namespace {

std::vector<MediaBackend>& registry() {
    static std::vector<MediaBackend> r;
    return r;
}

// The built-in WebM/VP9/Opus backend, registered lazily on first access
// rather than from a static initializer. Order across translation units is
// unspecified for static init, so a host registering at startup could
// otherwise land either side of the built-in depending on link order.
void ensureBuiltins() {
    static bool done = false;
    if (done) return;
    done = true;

    MediaBackend webm;
    webm.name = "webm";
    webm.priority = 0;
    webm.open = [](const std::string& path) -> std::unique_ptr<MediaSource> {
        auto demux = std::make_unique<WebMDemuxer>();
        if (!demux->open(path)) return nullptr;
        return demux;
    };
    webm.makeVideoDecoder = [](const TrackInfo& t) -> std::unique_ptr<VideoDecoder> {
        if (t.codec != Codec::VP9 && t.codec != Codec::VP8) return nullptr;
        return createVpxDecoder(t.codec, /*lowLatency=*/false);
    };
    webm.makeAudioDecoder = [](const TrackInfo& t) -> std::unique_ptr<AudioDecoder> {
        if (t.codec != Codec::Opus) return nullptr;
        return createOpusDecoder(t.sampleRate, t.channels);
    };
    registry().push_back(std::move(webm));
}

} // namespace

void registerMediaBackend(MediaBackend backend) {
    ensureBuiltins();
    auto& r = registry();
    // Stable within a priority: first registered is tried first, so a host
    // adding two backends at the same level gets the order it wrote them.
    auto at = std::upper_bound(r.begin(), r.end(), backend.priority,
                               [](int p, const MediaBackend& b) { return p > b.priority; });
    r.insert(at, std::move(backend));
}

const std::vector<MediaBackend>& mediaBackends() {
    ensureBuiltins();
    return registry();
}

} // namespace bro::video

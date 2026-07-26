#include "video/media_analysis.h"

#include "video/audio_decoder.h"
#include "video/media_backend.h"
#include "video/media_source.h"
#include "video/video_decoder.h"
#include "video/yuv_to_rgb.h"

#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace bro::video {

namespace {

// Open through the registry, highest priority first, and hand back the
// backend that claimed the file along with its source.
struct Opened {
    const MediaBackend* backend = nullptr;
    std::unique_ptr<MediaSource> source;
    explicit operator bool() const { return backend && source; }
};

Opened openAny(const std::string& path, TrackKind want) {
    for (const auto& backend : mediaBackends()) {
        if (!backend.open) continue;
        auto source = backend.open(path);
        if (!source) continue;
        for (const auto& t : source->tracks()) {
            if (t.kind != want) continue;
            return Opened{&backend, std::move(source)};
        }
        // Opened, but nothing of the kind we need — a backend further down
        // the list will not do better with the same container.
        return Opened{};
    }
    return Opened{};
}

const TrackInfo* firstTrack(const MediaSource& src, TrackKind kind) {
    for (const auto& t : src.tracks())
        if (t.kind == kind) return &t;
    return nullptr;
}

} // namespace

bool analyzeAudioPeaks(const std::string& path, int buckets, AudioPeaks& out) {
    if (buckets <= 0) return false;

    Opened opened = openAny(path, TrackKind::Audio);
    if (!opened) return false;
    const TrackInfo* track = firstTrack(*opened.source, TrackKind::Audio);
    if (!track) return false;

    auto dec = opened.backend->makeAudioDecoder
                   ? opened.backend->makeAudioDecoder(*track)
                   : nullptr;
    if (!dec) return false;

    TimeNs duration = track->durationNs;
    if (duration <= 0) {
        // A track with no declared duration (live capture, some MKVs) has no
        // fixed width to spread buckets across.
        for (const auto& t : opened.source->tracks())
            duration = std::max(duration, t.durationNs);
        if (duration <= 0) return false;
    }

    out.sampleRate = track->sampleRate;
    out.channels = track->channels;
    out.durationNs = duration;
    out.minv.assign(static_cast<size_t>(buckets), 0.0f);
    out.maxv.assign(static_cast<size_t>(buckets), 0.0f);
    out.rms.assign(static_cast<size_t>(buckets), 0.0f);
    std::vector<double> sumSq(static_cast<size_t>(buckets), 0.0);
    std::vector<uint64_t> counts(static_cast<size_t>(buckets), 0);

    opened.source->setActiveTracks({track->id});

    MediaPacket pkt;
    AudioFrame frame;
    while (opened.source->readPacket(pkt)) {
        if (pkt.trackId != track->id) continue;
        if (!dec->decode(pkt, frame)) continue;
        const uint32_t ch = frame.channels ? frame.channels : 1;
        const uint32_t rate = frame.sampleRate ? frame.sampleRate : out.sampleRate;
        if (!rate) continue;
        const size_t frames = frame.samples.size() / ch;
        if (!frames) continue;

        auto bucketOf = [&](size_t i) {
            const TimeNs t = frame.pts + static_cast<TimeNs>(i * 1000000000ULL / rate);
            auto b = static_cast<int64_t>(t * buckets / duration);
            return b < 0 ? 0 : (b >= buckets ? buckets - 1 : static_cast<int>(b));
        };

        // A codec frame is ~20 ms and a bucket is duration/buckets wide, so in
        // practice a whole packet lands in one bucket. Deciding that once and
        // reducing the packet in a tight loop takes the 64-bit divide off the
        // per-sample path — on a five-minute file that is 26 million of them.
        size_t i = 0;
        while (i < frames) {
            const int b = bucketOf(i);
            size_t end = frames;
            if (bucketOf(frames - 1) != b) {
                // Straddles a boundary: find where, by walking. Rare enough
                // that a linear scan beats being clever.
                end = i + 1;
                while (end < frames && bucketOf(end) == b) ++end;
            }

            float lo = out.minv[static_cast<size_t>(b)];
            float hi = out.maxv[static_cast<size_t>(b)];
            double sq = 0.0;
            const float* p = frame.samples.data() + i * ch;
            const float* stop = frame.samples.data() + end * ch;
            for (; p < stop; ++p) {
                const float s = *p;
                if (s < lo) lo = s;
                if (s > hi) hi = s;
                sq += double(s) * s;
            }
            out.minv[static_cast<size_t>(b)] = lo;
            out.maxv[static_cast<size_t>(b)] = hi;
            sumSq[static_cast<size_t>(b)] += sq;
            counts[static_cast<size_t>(b)] += (end - i) * ch;
            i = end;
        }
    }

    uint64_t total = 0;
    for (int b = 0; b < buckets; ++b) {
        if (counts[static_cast<size_t>(b)])
            out.rms[static_cast<size_t>(b)] = static_cast<float>(
                std::sqrt(sumSq[static_cast<size_t>(b)] /
                          double(counts[static_cast<size_t>(b)])));
        total += counts[static_cast<size_t>(b)];
    }
    return total > 0;
}

bool grabThumbnails(const std::string& path, int count, int height,
                    ThumbnailStrip& out) {
    if (count <= 0 || height <= 0) return false;

    Opened opened = openAny(path, TrackKind::Video);
    if (!opened) return false;
    const TrackInfo* track = firstTrack(*opened.source, TrackKind::Video);
    if (!track || !track->width || !track->height) return false;

    auto dec = opened.backend->makeVideoDecoder
                   ? opened.backend->makeVideoDecoder(*track)
                   : nullptr;
    if (!dec) return false;

    TimeNs duration = track->durationNs;
    for (const auto& t : opened.source->tracks())
        duration = std::max(duration, t.durationNs);

    const int tw = std::max(1, static_cast<int>(std::lround(
        double(height) * double(track->width) / double(track->height))));
    out.width = tw;
    out.height = height;
    out.count = count;
    out.times.assign(static_cast<size_t>(count), -1);
    out.rgba.assign(static_cast<size_t>(tw) * count * height * 4, 0);
    const int stride = tw * count * 4;

    opened.source->setActiveTracks({track->id});

    // How far past a keyframe we are willing to decode to reach the time we
    // actually asked for. Scaled by frame area, because that is what the walk
    // costs: a 720p file can afford to be exact, a 4K one cannot, and neither
    // should hold the caller for seconds. Below the floor the strip would be
    // all keyframes, which on a file with a ten-second GOP is the same picture
    // over and over.
    const int64_t area = int64_t(track->width) * track->height;
    const int walkBudget = static_cast<int>(
        std::clamp<int64_t>(30000000 / std::max<int64_t>(area, 1), 8, 240));

    // Decoding on from where we are beats seeking when the next thumbnail is
    // close — and it is exactly the case where a seek lands back on the same
    // keyframe and yields a duplicate picture.
    const TimeNs kResumeGapNs = 2000000000LL;   // 2 s

    int filled = 0;
    TimeNs pos = -1;    // pts of the last frame decoded, -1 before the first
    for (int i = 0; i < count; ++i) {
        // Sample from the middle of each slice, so the first thumbnail is a
        // frame of content rather than whatever black the file opens on.
        const TimeNs target =
            duration > 0
                ? static_cast<TimeNs>(duration * (2 * int64_t(i) + 1) / (2 * int64_t(count)))
                : 0;

        const bool needSeek = pos < 0 || target < pos || (target - pos) > kResumeGapNs;
        if (needSeek) {
            if (!opened.source->seekTo(target) && i > 0) {
                // Not seekable: everything after the first thumbnail would be
                // whatever decodes next, which is not a filmstrip.
                break;
            }
            dec->flush();
            pos = -1;
        }

        // Walk forward to the target, or as far as the budget allows.
        VideoFrame frame;
        bool got = false;
        MediaPacket pkt;
        auto walkTo = [&] {
            int walked = 0;
            bool drained = false;
            for (int guard = 0; guard < 4096; ++guard) {
                if (got && (frame.pts >= target || walked >= walkBudget)) break;
                if (!opened.source->readPacket(pkt)) {
                    // Out of packets is not out of pictures. A reordering
                    // codec is still holding its buffer, and that buffer is
                    // the tail of the file — the last thumbnails of the strip
                    // live in it and nothing else will ever produce them.
                    if (drained) break;
                    drained = true;
                    dec->drain();
                } else {
                    if (pkt.trackId != track->id) continue;
                    if (!dec->decode(pkt)) continue;
                }
                // Stop on the first picture at or after the target rather than
                // consuming everything the decoder offers: the overshoot would
                // be thrown away, and the next thumbnail resumes from here.
                while (dec->nextFrame(frame)) {
                    got = true;
                    ++walked;
                    if (frame.pts >= target) break;
                }
            }
        };
        walkTo();
        if (!got) break;
        pos = frame.pts;

        i420ToRgbaScaled(frame.y, frame.u, frame.v,
                         frame.strideY, frame.strideU, frame.strideV,
                         static_cast<int>(frame.width), static_cast<int>(frame.height),
                         out.rgba.data() + static_cast<size_t>(i) * tw * 4,
                         stride, tw, height);
        out.times[static_cast<size_t>(i)] = frame.pts;
        ++filled;
    }

    if (filled == 0) return false;
    if (filled < count) {
        // Short strip: pull the rows together so the image is really
        // (tw*filled) wide rather than a wide one with a blank tail.
        LOG_INFO("thumbnails: '%s' gave %d of %d frames", path.c_str(), filled, count);
        const size_t narrow = static_cast<size_t>(tw) * filled * 4;
        for (int row = 1; row < height; ++row)
            std::memmove(out.rgba.data() + narrow * row,
                         out.rgba.data() + static_cast<size_t>(stride) * row, narrow);
        out.count = filled;
        out.times.resize(static_cast<size_t>(filled));
        out.rgba.resize(narrow * height);
    }
    return true;
}

} // namespace bro::video

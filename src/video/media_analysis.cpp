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

// Clamp a window to a file of `duration` and say whether anything is left.
//
// `toNs` of 0 is the end, and both ends are clamped rather than refused,
// because a caller drawing a lane routinely asks for a span that runs a little
// past the last frame and the answer to that is the tail, not an error. What
// *is* refused is an empty span — a from at or past the end, or a to at or
// before the from — since spreading buckets across nothing would divide by it.
bool clampWindow(Window w, TimeNs duration, TimeNs& from, TimeNs& to) {
    if (duration <= 0) return false;
    from = w.fromNs > 0 ? w.fromNs : 0;
    to = w.toNs > 0 ? w.toNs : duration;
    if (from > duration) from = duration;
    if (to > duration) to = duration;
    return to > from;
}

} // namespace

bool analyzeAudioPeaks(const std::string& path, int buckets, AudioPeaks& out,
                       Window window) {
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

    TimeNs from = 0, to = 0;
    if (!clampWindow(window, duration, from, to)) return false;
    const TimeNs span = to - from;

    out.sampleRate = track->sampleRate;
    out.channels = track->channels;
    out.durationNs = duration;
    out.fromNs = from;
    out.toNs = to;
    out.minv.assign(static_cast<size_t>(buckets), 0.0f);
    out.maxv.assign(static_cast<size_t>(buckets), 0.0f);
    out.rms.assign(static_cast<size_t>(buckets), 0.0f);
    std::vector<double> sumSq(static_cast<size_t>(buckets), 0.0);
    std::vector<uint64_t> counts(static_cast<size_t>(buckets), 0);

    opened.source->setActiveTracks({track->id});

    // A seek that fails is not a failure here: reading from the start and
    // throwing away everything before `from` gives exactly the same buckets,
    // just slowly. Which is the trade the caller was trying to avoid, so it is
    // worth a line in the log — but a lane drawn late beats one not drawn.
    if (from > 0 && !opened.source->seekTo(from))
        LOG_INFO("peaks: '%s' will not seek; reading from the start", path.c_str());

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

        // -1 is before the window and `buckets` is past it, and neither is
        // clamped into an edge bucket. A seek lands on the packet boundary
        // *before* the time asked for, so there is always a run of samples in
        // front of the window; folding them into bucket 0 would draw the
        // second before the window as though it were the first one in it.
        auto bucketOf = [&](size_t i) {
            const TimeNs t = frame.pts + static_cast<TimeNs>(i * 1000000000ULL / rate);
            if (t < from) return -1;
            if (t >= to) return buckets;
            return static_cast<int>((t - from) * buckets / span);
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
            if (b < 0 || b >= buckets) { i = end; continue; }   // outside the window

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

        // Past the far end. Audio does not reorder, so the first frame that
        // begins at or after `to` is the last one worth decoding — and that is
        // what makes a window on a two-hour file cost a window rather than two
        // hours whichever end of it you asked for.
        if (frame.pts >= to) break;
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
                    ThumbnailStrip& out, Window window) {
    if (count <= 0 || height <= 0) return false;

    Opened opened = openAny(path, TrackKind::Video);
    if (!opened) return false;
    const TrackInfo* track = firstTrack(*opened.source, TrackKind::Video);
    if (!track || !track->width || !track->height) return false;

    auto dec = opened.backend->makeVideoDecoder
                   ? opened.backend->makeVideoDecoder(*track)
                   : nullptr;
    if (!dec) return false;

    // **The video track's own duration, not the container's.** They differ: a
    // recording routinely stops the audio a fraction of a second after the last
    // picture, and spreading the strip over the longer of the two puts the last
    // thumbnails past the end of the video, where the walk runs out of frames
    // and repeats whatever it last decoded.
    //
    // Falling back to the longest track — not to failure — for the reason
    // analyzeAudioPeaks does: Matroska keeps one duration for the whole file,
    // so a track that reports nothing of its own is ordinary rather than
    // broken, and a strip spread over the wrong span still beats no strip.
    TimeNs duration = track->durationNs;
    if (duration <= 0)
        for (const auto& t : opened.source->tracks())
            duration = std::max(duration, t.durationNs);

    // A window has to be clamped against something, and a file that will not
    // say how long it is offers nothing to clamp against — so a windowed grab
    // on one is refused rather than answered with a strip of whichever seconds
    // the walk happened to reach. The whole-file case keeps its own fallback
    // just above: no window, no clamping, and a span of zero puts every target
    // at the start, which is what it has always done.
    TimeNs from = 0, to = duration;
    if (window.fromNs > 0 || window.toNs > 0) {
        if (!clampWindow(window, duration, from, to)) return false;
    }
    const TimeNs span = to > from ? to - from : 0;

    // The aspect the strip is cut to is the DISPLAYED one. A sideways phone
    // clip is 1920x1080 on disk and 1080x1920 on screen, and a filmstrip that
    // followed the coded pair would lay landscape frames under a portrait
    // picture — which is what it used to do.
    const int rot = ((track->rotationDegrees % 360) + 360) % 360;
    const bool turned = (rot == 90 || rot == 270);
    const uint32_t dispW = turned ? track->height : track->width;
    const uint32_t dispH = turned ? track->width : track->height;

    const int tw = std::max(1, static_cast<int>(std::lround(
        double(height) * double(dispW) / double(dispH))));
    out.width = tw;
    out.height = height;
    out.count = count;
    out.rotationDegrees = rot;
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
            span > 0
                ? from + static_cast<TimeNs>(span * (2 * int64_t(i) + 1) / (2 * int64_t(count)))
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
                         stride, tw, height, rot);
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

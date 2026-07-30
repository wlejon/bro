#include "video/webm_encoder.h"

#include "video/rgba_to_yuv.h"

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>

#include <opus/opus.h>

#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace bro::video {

namespace {

unsigned int autoBitrateKbps(int width, int height, int fpsNum, int fpsDen) {
    // ~0.07 bits per pixel per frame is a reasonable VP9 visually-lossless
    // floor for synthetic / pixel-art content. We're encoding from RGBA in
    // a sprite/screen-recording context, not natural video, so we err on
    // the side of preserving sharp edges. Cap at 8 Mbit/s so an unconfigured
    // 4K capture doesn't produce a 50 MB/s file by accident.
    const double bps = double(width) * height *
                       (double(fpsNum) / std::max(1, fpsDen)) * 0.07;
    unsigned int kbps = static_cast<unsigned int>(bps / 1000.0);
    if (kbps < 200) kbps = 200;
    if (kbps > 8000) kbps = 8000;
    return kbps;
}

// Opus accepts only these input rates.
bool isOpusSampleRate(int sr) {
    return sr == 8000 || sr == 12000 || sr == 16000 || sr == 24000 || sr == 48000;
}

// Build a minimal RFC 7845 OpusHead identification header for codec_private.
// 19 bytes, channel mapping family 0 (mono / stereo).
std::vector<uint8_t> makeOpusHead(int channels, int inputSampleRate, int preSkip48k) {
    std::vector<uint8_t> h(19, 0);
    std::memcpy(h.data(), "OpusHead", 8);
    h[8]  = 1;                                              // version
    h[9]  = static_cast<uint8_t>(channels);
    h[10] = static_cast<uint8_t>(preSkip48k & 0xff);        // pre-skip LE
    h[11] = static_cast<uint8_t>((preSkip48k >> 8) & 0xff);
    h[12] = static_cast<uint8_t>(inputSampleRate & 0xff);   // input rate LE
    h[13] = static_cast<uint8_t>((inputSampleRate >> 8) & 0xff);
    h[14] = static_cast<uint8_t>((inputSampleRate >> 16) & 0xff);
    h[15] = static_cast<uint8_t>((inputSampleRate >> 24) & 0xff);
    // h[16..17]: output gain = 0; h[18]: channel mapping family = 0
    return h;
}

int qualityToDeadline(WebmEncoder::Quality q) {
    switch (q) {
        case WebmEncoder::Quality::Realtime: return VPX_DL_REALTIME;
        case WebmEncoder::Quality::Best:     return VPX_DL_BEST_QUALITY;
        case WebmEncoder::Quality::Good:
        default:                             return VPX_DL_GOOD_QUALITY;
    }
}

class WebmEncoderImpl final : public WebmEncoder {
public:
    WebmEncoderImpl() = default;
    ~WebmEncoderImpl() override { finish(); }

    bool open(const std::string& path, const Config& cfg, std::string* err) {
        cfg_ = cfg;
        // No size at all, with an audio rate, is a sound-only file: a legal
        // WebM with an Opus track and no video track in it. Any other missing
        // dimension is still the mistake it always was.
        hasVideo_ = !(cfg_.width == 0 && cfg_.height == 0 &&
                      cfg_.audioSampleRate > 0);
        if (hasVideo_ && (cfg_.width <= 0 || cfg_.height <= 0 ||
                          (cfg_.width  & 1) || (cfg_.height & 1))) {
            setErr("width/height must be positive and even "
                   "(omit both, with audioSampleRate set, for a sound-only file)");
            if (err) *err = lastErr_;
            return false;
        }
        if (cfg_.fpsNum <= 0 || cfg_.fpsDen <= 0) {
            setErr("fps numerator and denominator must be positive");
            if (err) *err = lastErr_;
            return false;
        }
        if (cfg_.keyframeIntervalSec <= 0) cfg_.keyframeIntervalSec = 2;
        if (cfg_.targetBitrateKbps <= 0) {
            cfg_.targetBitrateKbps = static_cast<int>(
                autoBitrateKbps(cfg_.width, cfg_.height, cfg_.fpsNum, cfg_.fpsDen));
        }
        deadline_ = qualityToDeadline(cfg_.quality);

        if (hasVideo_ && !initVideoCodec(err)) return false;

        // ---- WebM muxer ----
        if (!writer_.Open(path.c_str())) {
            setErr("MkvWriter::Open failed: " + path);
            if (err) *err = lastErr_;
            return false;
        }
        writerOpen_ = true;

        if (!segment_.Init(&writer_)) {
            setErr("Segment::Init failed");
            if (err) *err = lastErr_;
            return false;
        }
        segment_.set_mode(mkvmuxer::Segment::kFile);
        segment_.OutputCues(true);
        auto* info = segment_.GetSegmentInfo();
        info->set_writing_app("bro");

        if (hasVideo_ && !addVideoTrack(err)) return false;

        // ---- Optional Opus audio track ----
        if (cfg_.audioSampleRate > 0) {
            if (!isOpusSampleRate(cfg_.audioSampleRate)) {
                setErr("audioSampleRate must be 8000, 12000, 16000, 24000, or 48000");
                if (err) *err = lastErr_;
                return false;
            }
            if (cfg_.audioChannels < 1 || cfg_.audioChannels > 2) {
                setErr("audioChannels must be 1 or 2");
                if (err) *err = lastErr_;
                return false;
            }
            int oerr = 0;
            opusEnc_ = opus_encoder_create(cfg_.audioSampleRate,
                                           cfg_.audioChannels,
                                           OPUS_APPLICATION_AUDIO, &oerr);
            if (!opusEnc_ || oerr != OPUS_OK) {
                setErr(std::string("opus_encoder_create failed: ") +
                       opus_strerror(oerr));
                if (err) *err = lastErr_;
                return false;
            }
            opus_encoder_ctl(opusEnc_, OPUS_SET_BITRATE(
                cfg_.audioBitrateKbps * 1000));
            opus_encoder_ctl(opusEnc_, OPUS_SET_VBR(1));

            // Pre-skip = encoder lookahead, expressed at 48 kHz.
            opus_int32 lookahead = 0;
            opus_encoder_ctl(opusEnc_, OPUS_GET_LOOKAHEAD(&lookahead));
            const int preSkip48k = static_cast<int>(
                static_cast<int64_t>(lookahead) * 48000 / cfg_.audioSampleRate);
            audioPreSkip48k_ = preSkip48k;

            audioTrackId_ = segment_.AddAudioTrack(cfg_.audioSampleRate,
                                                   cfg_.audioChannels, 0);
            if (audioTrackId_ == 0) {
                setErr("AddAudioTrack failed");
                if (err) *err = lastErr_;
                return false;
            }
            auto* atrack = static_cast<mkvmuxer::AudioTrack*>(
                segment_.GetTrackByNumber(audioTrackId_));
            if (!atrack) {
                setErr("GetTrackByNumber(audio) returned null");
                if (err) *err = lastErr_;
                return false;
            }
            atrack->set_codec_id("A_OPUS");
            auto opusHead = makeOpusHead(cfg_.audioChannels,
                                         cfg_.audioSampleRate, preSkip48k);
            atrack->SetCodecPrivate(opusHead.data(),
                                    static_cast<int>(opusHead.size()));
            // codec_delay (pre-skip in nanoseconds at 48 kHz) and
            // seek_pre_roll (Opus standard: 80 ms) help decoders prime.
            atrack->set_codec_delay(static_cast<uint64_t>(
                static_cast<int64_t>(preSkip48k) * 1'000'000'000LL / 48000));
            atrack->set_seek_pre_roll(80'000'000ULL);

            // Frame size: 20 ms Opus packets — best size/latency tradeoff.
            audioFrameSize_ = cfg_.audioSampleRate / 50;
            audioPcmBuf_.reserve(static_cast<size_t>(audioFrameSize_) *
                                 cfg_.audioChannels);
            audioOutBuf_.resize(4000);  // libopus max recommended packet size
        }

        return true;
    }

    bool addFrameRGBA(const uint8_t* rgba, int srcStride) override {
        if (!hasVideo_) {
            setErr("this encoder has no video track: it was opened with no size");
            return false;
        }
        if (!codecInited_ || !writerOpen_) {
            setErr("encoder not open");
            return false;
        }
        if (!rgba) {
            setErr("null rgba pointer");
            return false;
        }
        if (srcStride <= 0) srcStride = cfg_.width * 4;

        rgbaToI420(rgba, srcStride, cfg_.width, cfg_.height,
                   image_.planes[VPX_PLANE_Y], image_.stride[VPX_PLANE_Y],
                   image_.planes[VPX_PLANE_U], image_.stride[VPX_PLANE_U],
                   image_.planes[VPX_PLANE_V], image_.stride[VPX_PLANE_V]);

        // Force a keyframe at the start so the file is decodable from frame 0
        // even if the encoder's internal heuristic would have deferred it.
        const vpx_enc_frame_flags_t flags =
            (frameIndex_ == 0) ? VPX_EFLAG_FORCE_KF : 0;

        if (vpx_codec_encode(&codec_, &image_,
                             static_cast<vpx_codec_pts_t>(frameIndex_), 1,
                             flags, deadline_) != VPX_CODEC_OK) {
            setErr(std::string("vpx_codec_encode failed: ") +
                   (vpx_codec_error_detail(&codec_) ? vpx_codec_error_detail(&codec_) : ""));
            return false;
        }
        ++frameIndex_;
        return drainPackets();
    }

    bool addAudioFramesPCM(const float* interleaved, int frameCount) override {
        if (!opusEnc_) return true;  // no audio track configured — silent no-op
        if (finished_) {
            setErr("addAudioFramesPCM after finish");
            return false;
        }
        if (!interleaved || frameCount <= 0) return true;

        const int ch = cfg_.audioChannels;
        const size_t total = static_cast<size_t>(frameCount) * ch;
        size_t consumed = 0;
        while (consumed < total) {
            const size_t want = static_cast<size_t>(audioFrameSize_) * ch
                              - audioPcmBuf_.size();
            const size_t take = std::min(want, total - consumed);
            audioPcmBuf_.insert(audioPcmBuf_.end(),
                                interleaved + consumed,
                                interleaved + consumed + take);
            consumed += take;
            if (audioPcmBuf_.size() ==
                static_cast<size_t>(audioFrameSize_) * ch) {
                if (!encodeOpusFrame(audioPcmBuf_.data(), audioFrameSize_)) {
                    return false;
                }
                audioPcmBuf_.clear();
            }
        }
        return true;
    }

    bool finish() override {
        if (finished_) return true;
        finished_ = true;

        bool ok = true;
        if (codecInited_) {
            // Flush: feed null images until no more packets emerge.
            while (true) {
                if (vpx_codec_encode(&codec_, nullptr,
                                     static_cast<vpx_codec_pts_t>(frameIndex_), 1,
                                     0, deadline_) != VPX_CODEC_OK) {
                    setErr("vpx_codec_encode flush failed");
                    ok = false;
                    break;
                }
                bool gotAny = drainPackets();
                if (!gotAny) break;
                ++frameIndex_;
            }
        }

        // Drain any partial Opus frame, zero-padded up to a full packet. Tag
        // the block with DiscardPadding so the decoder drops the trailing
        // silence and the track ends at the intended sample count. The amount
        // to discard is the zero fill MINUS the encoder pre-skip: the decoder
        // only emits (fed - preSkip) samples, so the last preSkip samples of
        // real audio are still in the pipeline and the tail is already that
        // much shorter. Computed in the Opus 48 kHz output domain, clamped ≥ 0.
        if (opusEnc_ && !audioPcmBuf_.empty()) {
            const int ch = cfg_.audioChannels;
            const int validPerCh = static_cast<int>(audioPcmBuf_.size() / ch);
            const int padPerCh = audioFrameSize_ - validPerCh;   // zero-filled tail
            audioPcmBuf_.resize(static_cast<size_t>(audioFrameSize_) * ch, 0.0f);
            const int64_t padPer48k =
                static_cast<int64_t>(padPerCh) * 48000 / cfg_.audioSampleRate;
            int64_t discard48k = padPer48k - audioPreSkip48k_;
            if (discard48k < 0) discard48k = 0;
            const int64_t discardNs = discard48k * 1'000'000'000LL / 48000;
            if (!encodeOpusFrame(audioPcmBuf_.data(), audioFrameSize_, discardNs))
                ok = false;
            audioPcmBuf_.clear();
        }

        if (writerOpen_) {
            if (!flushPending()) ok = false;
            // Declare the real duration. Left to itself, libwebm computes the
            // segment Duration from the last block's TIMESTAMP, which is where
            // the final frame starts, not where it ends — a 5-frame 5 fps clip
            // came out 0.8s instead of 1.0s and a looping player dropped the
            // last frame. Segment::set_duration overrides that computation;
            // the value is in timecode-scale units (ns per tick, 1 ms default).
            if (streamEndNs_ > 0) {
                const uint64_t scale = segment_.GetSegmentInfo()->timecode_scale();
                if (scale > 0)
                    segment_.set_duration(static_cast<double>(streamEndNs_) /
                                          static_cast<double>(scale));
            }
            if (!segment_.Finalize()) {
                setErr("Segment::Finalize failed");
                ok = false;
            }
            writer_.Close();
            writerOpen_ = false;
        }
        if (imgInited_) {
            vpx_img_free(&image_);
            imgInited_ = false;
        }
        if (codecInited_) {
            vpx_codec_destroy(&codec_);
            codecInited_ = false;
        }
        if (opusEnc_) {
            opus_encoder_destroy(opusEnc_);
            opusEnc_ = nullptr;
        }
        return ok;
    }

    const std::string& lastError() const override { return lastErr_; }
    int framesWritten() const override { return framesWritten_; }

private:
    // The video half of open(), lifted out so a sound-only file simply does
    // not run it. Everything below the muxer is shared.
    bool initVideoCodec(std::string* err) {
        // ---- libvpx encoder ----
        const auto* iface = vpx_codec_vp9_cx();
        vpx_codec_enc_cfg_t vc{};
        if (vpx_codec_enc_config_default(iface, &vc, 0) != VPX_CODEC_OK) {
            setErr("vpx_codec_enc_config_default failed");
            if (err) *err = lastErr_;
            return false;
        }
        vc.g_w = cfg_.width;
        vc.g_h = cfg_.height;
        vc.g_timebase.num = cfg_.fpsDen;
        vc.g_timebase.den = cfg_.fpsNum;
        vc.rc_target_bitrate = static_cast<unsigned int>(cfg_.targetBitrateKbps);
        vc.rc_end_usage = VPX_VBR;
        vc.kf_mode = VPX_KF_AUTO;
        vc.kf_max_dist = static_cast<unsigned int>(cfg_.keyframeIntervalSec) *
                         static_cast<unsigned int>(cfg_.fpsNum) /
                         static_cast<unsigned int>(cfg_.fpsDen);
        if (vc.kf_max_dist == 0) vc.kf_max_dist = 1;
        vc.kf_min_dist = 0;
        vc.g_threads = (cfg_.threads > 0) ? static_cast<unsigned int>(cfg_.threads) : 1;
        vc.g_pass = VPX_RC_ONE_PASS;
        vc.g_lag_in_frames = 0;       // no look-ahead — keeps encode synchronous
        vc.g_error_resilient = 0;

        if (vpx_codec_enc_init(&codec_, iface, &vc, 0) != VPX_CODEC_OK) {
            setErr(std::string("vpx_codec_enc_init failed: ") +
                   (vpx_codec_error_detail(&codec_) ? vpx_codec_error_detail(&codec_) : ""));
            if (err) *err = lastErr_;
            return false;
        }
        codecInited_ = true;

        // Realtime preset: row-MT + cpu-used 7 (fastest); Good: cpu-used 1.
        const int cpuUsed =
            (cfg_.quality == Quality::Realtime) ? 7 :
            (cfg_.quality == Quality::Best)     ? 0 : 1;
        vpx_codec_control(&codec_, VP8E_SET_CPUUSED, cpuUsed);
        if (cfg_.quality == Quality::Realtime) {
            vpx_codec_control(&codec_, VP9E_SET_ROW_MT, 1);
        }

        if (!vpx_img_alloc(&image_, VPX_IMG_FMT_I420, cfg_.width, cfg_.height, 1)) {
            setErr("vpx_img_alloc failed");
            if (err) *err = lastErr_;
            return false;
        }
        imgInited_ = true;
        return true;
    }

    bool addVideoTrack(std::string* err) {
        videoTrackId_ = segment_.AddVideoTrack(cfg_.width, cfg_.height, 1);
        if (videoTrackId_ == 0) {
            setErr("AddVideoTrack failed");
            if (err) *err = lastErr_;
            return false;
        }
        auto* track = static_cast<mkvmuxer::VideoTrack*>(
            segment_.GetTrackByNumber(videoTrackId_));
        if (!track) {
            setErr("GetTrackByNumber returned null");
            if (err) *err = lastErr_;
            return false;
        }
        // Hardcode the codec id literal — the static const char arrays in
        // mkvmuxer::Tracks aren't exported by the vcpkg unofficial-libwebm
        // build, so referencing kVp9CodecId fails to link.
        track->set_codec_id("V_VP9");
        const double frameRate = double(cfg_.fpsNum) / double(cfg_.fpsDen);
        track->set_frame_rate(frameRate);
        // Rotation goes in the Video Projection element's pose roll — a
        // rectangular projection rolled about the forward vector, which is
        // what a sideways-recorded clip is. Written only when there is one, so
        // an ordinary file has no Projection element and is unchanged.
        const int rot = ((cfg_.rotationDegrees % 360) + 360) % 360;
        if (rot == 90 || rot == 180 || rot == 270) {
            mkvmuxer::Projection projection;
            projection.set_type(mkvmuxer::Projection::kRectangular);
            projection.set_pose_yaw(0.0f);
            projection.set_pose_pitch(0.0f);
            projection.set_pose_roll(static_cast<float>(rot));
            if (!track->SetProjection(projection)) {
                setErr("SetProjection failed");
                if (err) *err = lastErr_;
                return false;
            }
        }
        // default_duration is in nanoseconds.
        track->set_default_duration(static_cast<uint64_t>(1.0e9 / frameRate));
        return true;
    }

    // False when this file has no video track at all.
    bool hasVideo_ = true;

    void setErr(const std::string& s) { lastErr_ = s; }

    // Pull all available packets from the encoder and mux them. Returns true
    // if at least one packet was consumed (used by the flush loop to know
    // whether to feed another null image).
    bool encodeOpusFrame(const float* pcm, int frameSize,
                         int64_t discardPaddingNs = 0) {
        const int n = opus_encode_float(opusEnc_, pcm, frameSize,
                                        audioOutBuf_.data(),
                                        static_cast<opus_int32>(audioOutBuf_.size()));
        if (n < 0) {
            setErr(std::string("opus_encode_float failed: ") + opus_strerror(n));
            return false;
        }
        if (n == 0) {
            audioFramesEncoded_ += frameSize;
            return true;
        }
        const uint64_t pts_ns =
            static_cast<uint64_t>(audioFramesEncoded_) * 1'000'000'000ULL /
            static_cast<uint64_t>(cfg_.audioSampleRate);
        enqueuePacket(pts_ns, audioTrackId_, /*isKey=*/true,
                      audioOutBuf_.data(), static_cast<size_t>(n), discardPaddingNs);
        audioFramesEncoded_ += frameSize;
        return true;
    }

    // libwebm requires globally monotonic timestamps, but interleaved video
    // and audio production naturally goes out of order (video PTS jumps by
    // 1/fps while audio PTS jumps by 20 ms). Buffer all packets and sort
    // before muxing during finish().
    struct PendingPacket {
        uint64_t pts_ns;
        uint64_t trackId;
        bool isKey;
        int64_t discardPaddingNs;
        std::vector<uint8_t> data;
    };
    void enqueuePacket(uint64_t pts_ns, uint64_t trackId, bool isKey,
                       const uint8_t* buf, size_t len,
                       int64_t discardPaddingNs = 0) {
        PendingPacket p;
        p.pts_ns = pts_ns;
        p.trackId = trackId;
        p.isKey = isKey;
        p.discardPaddingNs = discardPaddingNs;
        p.data.assign(buf, buf + len);
        pending_.push_back(std::move(p));
        // Where this packet ENDS, not where it starts. libwebm derives the
        // segment Duration from the last block's timestamp, which is the start
        // of the final frame — so a clip came out one frame short and a looping
        // player dropped its last frame. Tracked here, for both tracks, so
        // finish() can write the real end of the stream.
        const uint64_t durNs = (trackId == audioTrackId_ && cfg_.audioSampleRate > 0)
            ? (static_cast<uint64_t>(audioFrameSize_) * 1'000'000'000ULL /
               static_cast<uint64_t>(cfg_.audioSampleRate))
            : (1'000'000'000ULL * static_cast<uint64_t>(cfg_.fpsDen) /
               static_cast<uint64_t>(cfg_.fpsNum));
        const uint64_t end = pts_ns + durNs;
        if (end > streamEndNs_) streamEndNs_ = end;
    }
    bool flushPending() {
        std::stable_sort(pending_.begin(), pending_.end(),
            [](const PendingPacket& a, const PendingPacket& b) {
                return a.pts_ns < b.pts_ns;
            });
        for (auto& p : pending_) {
            const bool wrote = p.discardPaddingNs > 0
                ? segment_.AddFrameWithDiscardPadding(
                      p.data.data(), p.data.size(), p.discardPaddingNs,
                      p.trackId, p.pts_ns, p.isKey)
                : segment_.AddFrame(p.data.data(), p.data.size(),
                                    p.trackId, p.pts_ns, p.isKey);
            if (!wrote) {
                setErr("Segment::AddFrame failed during flush");
                return false;
            }
            if (p.trackId == videoTrackId_) ++framesWritten_;
        }
        pending_.clear();
        return true;
    }

    bool drainPackets() {
        bool gotAny = false;
        const void* iter = nullptr;
        const vpx_codec_cx_pkt_t* pkt = nullptr;
        while ((pkt = vpx_codec_get_cx_data(&codec_, &iter)) != nullptr) {
            if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            const bool isKey = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            // Convert PTS in timebase units (1 unit = fpsDen/fpsNum sec) to ns.
            const uint64_t pts_ns =
                (static_cast<uint64_t>(pkt->data.frame.pts) *
                 static_cast<uint64_t>(cfg_.fpsDen) * 1'000'000'000ULL) /
                static_cast<uint64_t>(cfg_.fpsNum);
            enqueuePacket(pts_ns, videoTrackId_, isKey,
                          static_cast<const uint8_t*>(pkt->data.frame.buf),
                          pkt->data.frame.sz);
            gotAny = true;
        }
        return gotAny;
    }

    Config cfg_{};
    // End of the latest packet on any track, in ns — the file's true duration.
    uint64_t streamEndNs_ = 0;
    int deadline_ = VPX_DL_GOOD_QUALITY;
    vpx_codec_ctx_t codec_{};
    vpx_image_t image_{};
    bool codecInited_ = false;
    bool imgInited_ = false;

    mkvmuxer::MkvWriter writer_;
    mkvmuxer::Segment segment_;
    bool writerOpen_ = false;
    uint64_t videoTrackId_ = 0;
    uint64_t audioTrackId_ = 0;

    OpusEncoder* opusEnc_ = nullptr;
    int audioPreSkip48k_ = 0;           // encoder lookahead, in 48 kHz samples
    int audioFrameSize_ = 0;            // samples per channel per Opus packet
    int64_t audioFramesEncoded_ = 0;    // running PTS in input samples
    std::vector<float> audioPcmBuf_;    // interleaved staging for partial fills
    std::vector<uint8_t> audioOutBuf_;

    std::vector<PendingPacket> pending_;

    uint64_t frameIndex_ = 0;
    int framesWritten_ = 0;
    bool finished_ = false;
    std::string lastErr_;
};

} // namespace

std::unique_ptr<WebmEncoder> WebmEncoder::create(const std::string& path,
                                                 const Config& cfg,
                                                 std::string* err) {
    auto enc = std::make_unique<WebmEncoderImpl>();
    if (!enc->open(path, cfg, err)) return nullptr;
    return enc;
}

} // namespace bro::video

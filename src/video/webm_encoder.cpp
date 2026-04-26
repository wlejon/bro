#include "video/webm_encoder.h"

#include "video/rgba_to_yuv.h"

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>

#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>

#include <algorithm>
#include <cstring>

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
        if (cfg_.width <= 0 || cfg_.height <= 0 ||
            (cfg_.width  & 1) || (cfg_.height & 1)) {
            setErr("width/height must be positive and even");
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
        // default_duration is in nanoseconds.
        track->set_default_duration(static_cast<uint64_t>(1.0e9 / frameRate));

        return true;
    }

    bool addFrameRGBA(const uint8_t* rgba, int srcStride) override {
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

        if (writerOpen_) {
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
        return ok;
    }

    const std::string& lastError() const override { return lastErr_; }
    int framesWritten() const override { return framesWritten_; }

private:
    void setErr(const std::string& s) { lastErr_ = s; }

    // Pull all available packets from the encoder and mux them. Returns true
    // if at least one packet was consumed (used by the flush loop to know
    // whether to feed another null image).
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
            if (!segment_.AddFrame(static_cast<const uint8_t*>(pkt->data.frame.buf),
                                   pkt->data.frame.sz, videoTrackId_,
                                   pts_ns, isKey)) {
                setErr("Segment::AddFrame failed");
                return gotAny;
            }
            ++framesWritten_;
            gotAny = true;
        }
        return gotAny;
    }

    Config cfg_{};
    int deadline_ = VPX_DL_GOOD_QUALITY;
    vpx_codec_ctx_t codec_{};
    vpx_image_t image_{};
    bool codecInited_ = false;
    bool imgInited_ = false;

    mkvmuxer::MkvWriter writer_;
    mkvmuxer::Segment segment_;
    bool writerOpen_ = false;
    uint64_t videoTrackId_ = 0;

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

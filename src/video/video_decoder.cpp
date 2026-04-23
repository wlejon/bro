#include "video/video_decoder.h"

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_image.h>

namespace bro::video {

namespace {

class VpxDecoder final : public VideoDecoder {
public:
    VpxDecoder(Codec codec, bool lowLatency) : codec_(codec), lowLatency_(lowLatency) {}

    ~VpxDecoder() override {
        if (inited_) vpx_codec_destroy(&ctx_);
    }

    bool init() {
        const vpx_codec_iface_t* iface = nullptr;
        if (codec_ == Codec::VP9) iface = vpx_codec_vp9_dx();
        else if (codec_ == Codec::VP8) iface = vpx_codec_vp8_dx();
        else return false;

        vpx_codec_dec_cfg_t cfg{};
        cfg.threads = 4;
        vpx_codec_flags_t flags = 0;
        if (vpx_codec_dec_init(&ctx_, iface, &cfg, flags) != VPX_CODEC_OK) return false;
        inited_ = true;

        if (lowLatency_) {
            // In VP9 this disables frame parallel decoding's reorder latency.
            vpx_codec_control(&ctx_, VP9D_SET_ROW_MT, 1);
        }
        return true;
    }

    bool decode(const MediaPacket& pkt) override {
        if (!inited_ && !init()) return false;
        if (!pkt.data || pkt.data->empty()) return true;

        const int64_t pts = pkt.pts;
        const auto err = vpx_codec_decode(&ctx_, pkt.data->data(),
                                          static_cast<unsigned int>(pkt.data->size()),
                                          reinterpret_cast<void*>(static_cast<intptr_t>(pts ? 1 : 1)),
                                          0);
        if (err != VPX_CODEC_OK) {
            needsKey_ = true;
            return false;
        }
        // libvpx doesn't thread pts through user_priv reliably across versions;
        // stash the latest pts for the upcoming nextFrame() pulls.
        lastPts_ = pts;
        iter_ = nullptr;
        return true;
    }

    bool nextFrame(VideoFrame& out) override {
        if (!inited_) return false;
        vpx_image_t* img = vpx_codec_get_frame(&ctx_, &iter_);
        if (!img) return false;

        out.width = img->d_w;
        out.height = img->d_h;
        out.pts = lastPts_;
        out.y = img->planes[VPX_PLANE_Y];
        out.u = img->planes[VPX_PLANE_U];
        out.v = img->planes[VPX_PLANE_V];
        out.strideY = img->stride[VPX_PLANE_Y];
        out.strideU = img->stride[VPX_PLANE_U];
        out.strideV = img->stride[VPX_PLANE_V];
        out.storage.reset();
        needsKey_ = false;
        return true;
    }

    bool needsKeyframe() const override { return needsKey_; }

private:
    Codec codec_;
    bool lowLatency_;
    bool inited_ = false;
    vpx_codec_ctx_t ctx_{};
    const void* iter_ = nullptr;
    TimeNs lastPts_ = 0;
    bool needsKey_ = false;
};

} // namespace

std::unique_ptr<VideoDecoder> createVpxDecoder(Codec codec, bool lowLatency) {
    return std::make_unique<VpxDecoder>(codec, lowLatency);
}

} // namespace bro::video

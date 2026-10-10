#include "render/webp_image.h"

#include "broimage/codec.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace bro::render {

namespace {

bool isWebP(const uint8_t* d, std::size_t n) {
    return n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0;
}

bool isAnimatedWebP(const uint8_t* d, std::size_t n) {
    WebPBitstreamFeatures f;
    return WebPGetFeatures(d, n, &f) == VP8_STATUS_OK && f.has_animation;
}

// An animated WebP through libwebp's compositor (anim_decode.c), which keeps
// the canvas and the previous one: frames come out composed, blended and
// disposed, a frame at a time.
class WebPFrames final : public broimage::FrameDecoder {
public:
    static std::unique_ptr<WebPFrames> open(broimage::SharedBytes bytes, std::string* why) {
        auto f = std::unique_ptr<WebPFrames>(new WebPFrames(std::move(bytes)));
        WebPAnimDecoderOptions opts;
        if (!WebPAnimDecoderOptionsInit(&opts)) return nullptr;
        opts.color_mode = MODE_RGBA;  // straight alpha, like every decode here
        opts.use_threads = 0;
        WebPData data{f->bytes_->data(), f->bytes_->size()};
        f->dec_ = WebPAnimDecoderNew(&data, &opts);
        WebPAnimInfo info;
        if (!f->dec_ || !WebPAnimDecoderGetInfo(f->dec_, &info) || info.canvas_width == 0 ||
            info.canvas_height == 0 || info.frame_count == 0) {
            if (why) *why = "the WebP animation could not be read";
            return nullptr;
        }
        f->w_ = int(info.canvas_width);
        f->h_ = int(info.canvas_height);
        f->frames_ = int(info.frame_count);
        f->loops_ = int(info.loop_count);
        if (const WebPDemuxer* demux = WebPAnimDecoderGetDemuxer(f->dec_)) {
            WebPIterator it;
            if (WebPDemuxGetFrame(demux, 1, &it)) {
                do f->delays_.push_back(it.duration);
                while (WebPDemuxNextFrame(&it));
                WebPDemuxReleaseIterator(&it);
            }
        }
        f->delays_.resize(std::size_t(f->frames_), 0);
        return f;
    }

    ~WebPFrames() override {
        if (dec_) WebPAnimDecoderDelete(dec_);
    }

    int width() const override { return w_; }
    int height() const override { return h_; }
    int frame_count() const override { return frames_; }
    int loop_count() const override { return loops_; }
    const std::vector<int>& delays_ms() const override { return delays_; }
    int index() const override { return index_; }

    bool rewind() override {
        WebPAnimDecoderReset(dec_);
        index_ = 0;
        lastTimestamp_ = 0;
        return true;
    }

    bool next(broimage::AnimationFrame& out, std::string* error) override {
        if (error) error->clear();
        if (!WebPAnimDecoderHasMoreFrames(dec_)) return false;
        uint8_t* buf = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(dec_, &buf, &timestamp) || !buf) {
            if (error) *error = "a WebP frame could not be decoded";
            return false;
        }
        out.rgba.assign(buf, buf + std::size_t(w_) * h_ * 4);
        out.delay_ms = timestamp - lastTimestamp_;
        lastTimestamp_ = timestamp;
        ++index_;
        return true;
    }

private:
    explicit WebPFrames(broimage::SharedBytes bytes) : bytes_(std::move(bytes)) {}

    broimage::SharedBytes bytes_;  // the decoder reads them in place
    WebPAnimDecoder* dec_ = nullptr;
    int w_ = 0, h_ = 0, frames_ = 0, loops_ = 0, index_ = 0, lastTimestamp_ = 0;
    std::vector<int> delays_;
};

bool codecSniff(const uint8_t* d, std::size_t n) { return isWebP(d, n); }

bool codecProbe(const uint8_t* d, std::size_t n, int& w, int& h, int& channels) {
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK || f.width <= 0 || f.height <= 0) return false;
    w = f.width;
    h = f.height;
    channels = f.has_alpha ? 4 : 3;
    return true;
}

bool codecDecode(const uint8_t* d, std::size_t n, broimage::Image& out, std::string* why) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (decodeWebP(d, n, w, h, rgba)) {
        out.width = w;
        out.height = h;
        out.channels = 4;
        out.pixels = std::move(rgba);
        return true;
    }
    // WebPDecode reads no animation: its first frame, composed.
    if (isAnimatedWebP(d, n)) {
        auto frames = WebPFrames::open(std::make_shared<const std::vector<uint8_t>>(d, d + n), why);
        broimage::AnimationFrame f;
        if (frames && frames->next(f, why)) {
            out.width = frames->width();
            out.height = frames->height();
            out.channels = 4;
            out.pixels = std::move(f.rgba);
            return true;
        }
    }
    if (why && why->empty()) *why = "the WebP could not be decoded";
    return false;
}

std::unique_ptr<broimage::FrameDecoder> codecOpenFrames(broimage::SharedBytes bytes, std::string* why) {
    if (!bytes || !isAnimatedWebP(bytes->data(), bytes->size())) return nullptr;  // a still: one frame
    return WebPFrames::open(std::move(bytes), why);
}

}  // namespace

void registerWebPCodec() {
    broimage::Codec c;
    c.name = "webp";
    c.sniff = codecSniff;
    c.probe = codecProbe;
    c.decode = codecDecode;
    c.open_frames = codecOpenFrames;
    broimage::register_codec(c);
}

bool decodeWebP(const void* data, std::size_t len,
                int& width, int& height, std::vector<uint8_t>& out) {
    if (!data || len == 0) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);

    // WebPGetInfo doubles as the format check: it validates the RIFF/WEBP
    // container and the VP8/VP8L/VP8X chunk header, so non-WebP bytes and
    // truncated files are both rejected here rather than part-decoded. That
    // is why there is no separate signature sniff.
    int w = 0, h = 0;
    if (!WebPGetInfo(bytes, len, &w, &h)) return false;
    if (w <= 0 || h <= 0) return false;

    uint8_t* pixels = WebPDecodeRGBA(bytes, len, &w, &h);
    if (!pixels) return false;

    // Copy into the caller's vector and release libwebp's buffer immediately.
    // Handing the raw pointer out instead would make every caller responsible
    // for calling WebPFree (not free()) on exactly the right paths, which is
    // a trap for the price of one memcpy on a decode that already cost far
    // more than that.
    const std::size_t bytesOut = static_cast<std::size_t>(w) * h * 4;
    out.assign(pixels, pixels + bytesOut);
    WebPFree(pixels);

    width = w;
    height = h;
    return true;
}

bool decodeWebPHeader(const void* data, std::size_t len, int& width, int& height) {
    if (!data || len == 0) return false;
    int w = 0, h = 0;
    if (!WebPGetInfo(static_cast<const uint8_t*>(data), len, &w, &h)) return false;
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    return true;
}

bool decodeWebPFile(const std::string& path,
                    int& width, int& height, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    std::vector<uint8_t> bytes;
    // A WebP that isn't one is the common case here (this is a fallback, so
    // most calls arrive with PNG/JPEG bytes); read the whole file anyway
    // rather than sniffing first, since the header check inside decodeWebP is
    // the authoritative one and these files are small.
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size <= 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    bytes.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return false;

    return decodeWebP(bytes.data(), bytes.size(), width, height, out);
}

} // namespace bro::render

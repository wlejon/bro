#include "render/image_store.h"

#include "svg/svg_renderer.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

#include "broimage/decode.h"

#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif

#include <include/codec/SkCodec.h>
#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <thread>
#include <unordered_map>

namespace bro::render {

namespace {

uint64_t nextPixelsId() {
    // Shares no range with anything else that mints SharedPixels ids: those
    // count up from 1, these from the top bit.
    static std::atomic<uint64_t> counter{1};
    return (uint64_t(1) << 62) | counter.fetch_add(1, std::memory_order_relaxed);
}

void orientInPlace(DecodedImage& out, int orientation) {
    if (orientation <= 1 || orientation > 8 || out.rgba.empty()) return;
    broimage::Image tmp;
    tmp.width = out.width;
    tmp.height = out.height;
    tmp.channels = 4;
    tmp.pixels = std::move(out.rgba);
    broimage::apply_exif_orientation(tmp, static_cast<broimage::ExifOrientation>(orientation));
    out.width = tmp.width;
    out.height = tmp.height;
    out.rgba = std::move(tmp.pixels);
}

int exifOrientationOf(const uint8_t* bytes, size_t len) {
    const int o = static_cast<int>(broimage::read_exif_orientation(bytes, len));
    return (o >= 1 && o <= 8) ? o : 1;
}

// Skia's codecs, straight into RGBA8 unpremultiplied. Whether this pinned
// Skia carries a given codec differs by platform; broimage is the fallback.
bool decodeWithSkCodec(const uint8_t* bytes, size_t len, DecodedImage& out) {
    sk_sp<SkData> data = SkData::MakeWithoutCopy(bytes, len);
    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(data);
    if (!codec) return false;
    const SkImageInfo src = codec->getInfo();
    if (src.width() <= 0 || src.height() <= 0) return false;
    const SkImageInfo info = SkImageInfo::Make(src.width(), src.height(), kRGBA_8888_SkColorType,
                                               kUnpremul_SkAlphaType);
    std::vector<uint8_t> px(static_cast<size_t>(src.width()) * src.height() * 4);
    const SkCodec::Result r = codec->getPixels(info, px.data(), static_cast<size_t>(src.width()) * 4);
    if (r != SkCodec::kSuccess && r != SkCodec::kIncompleteInput) return false;
    out.width = src.width();
    out.height = src.height();
    out.rgba = std::move(px);
    return true;
}

}  // namespace

SharedPixels sharedPixelsOf(const std::shared_ptr<const DecodedImage>& img) {
    SharedPixels px;
    if (!img || img->rgba.empty() || img->width <= 0 || img->height <= 0) return px;
    px.id = img->id;
    px.width = img->width;
    px.height = img->height;
    px.rgba = img->rgba.data();
    px.owner = img;
    return px;
}

bool decodeImageData(const uint8_t* bytes, size_t len, bool orient, DecodedImage& out,
                     std::string& err) {
    out = DecodedImage{};
    out.id = nextPixelsId();
    if (!bytes || len == 0) {
        err = "empty image data";
        return false;
    }
    const char* chars = reinterpret_cast<const char*>(bytes);
    if (svg::looksLikeSvg(chars, len)) {
        // Markup for the painter (it draws vectors at any size), and a raster
        // at the intrinsic size for the consumers that need pixels. An SVG
        // without an intrinsic size is still a usable image; it has no raster.
        out.isSvg = true;
        out.svgMarkup.assign(chars, len);
        float sw = 0, sh = 0;
        svg::svgIntrinsicSize(chars, len, sw, sh);
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (svg::rasterizeSvgMarkup(chars, len, 0, 0, w, h, rgba)) {
            out.width = w;
            out.height = h;
            out.rgba = std::move(rgba);
        } else {
            out.width = static_cast<int>(sw);
            out.height = static_cast<int>(sh);
        }
        out.oriented = orient;
        return true;
    }

    bool ok = false;
#if BRO_WITH_WEBP
    // bro's own WebP decoder first, on every platform: a Skia built with
    // libwebp would otherwise take the bytes on one OS and not another.
    {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (decodeWebP(bytes, len, w, h, rgba)) {
            out.width = w;
            out.height = h;
            out.rgba = std::move(rgba);
            ok = true;
        }
    }
#endif
    if (!ok) ok = decodeWithSkCodec(bytes, len, out);
    if (!ok) {
        broimage::Image decoded;
        std::string decErr;
        if (broimage::decode_memory(bytes, len, decoded, &decErr)) {
            out.width = decoded.width;
            out.height = decoded.height;
            out.rgba = std::move(decoded.pixels);
            ok = true;
        } else {
            // A failed decode is a BROKEN image: broimage's white fallback
            // pixel is not adopted.
            err = decErr.empty() ? std::string("no decoder accepted the bytes") : decErr;
            return false;
        }
    }
    out.orientation = exifOrientationOf(bytes, len);
    out.oriented = orient;
    if (orient) orientInPlace(out, out.orientation);
    return true;
}

bool probeImageData(const uint8_t* bytes, size_t len, bool orient, int& width, int& height,
                    int& orientation, bool& isSvg) {
    width = height = 0;
    orientation = 1;
    isSvg = false;
    if (!bytes || len == 0) return false;
    const char* chars = reinterpret_cast<const char*>(bytes);
    if (svg::looksLikeSvg(chars, len)) {
        float sw = 0, sh = 0;
        svg::svgIntrinsicSize(chars, len, sw, sh);
        width = static_cast<int>(sw);
        height = static_cast<int>(sh);
        isSvg = true;
        return true;
    }
    int w = 0, h = 0, c = 0;
    bool ok = broimage::probe_dimensions_memory(bytes, len, &w, &h, &c);
#if BRO_WITH_WEBP
    if (!ok) ok = decodeWebPHeader(bytes, len, w, h);
#endif
    if (!ok) return false;
    orientation = exifOrientationOf(bytes, len);
    if (orient && orientation >= 5) std::swap(w, h);
    width = w;
    height = h;
    return true;
}

// ---------------------------------------------------------------------------
// ImageRequest
// ---------------------------------------------------------------------------

bool ImageRequest::wait(double timeoutMs) const {
    if (settled()) return true;
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, std::chrono::duration<double, std::milli>(timeoutMs),
                        [this] { return settled(); });
}

// ---------------------------------------------------------------------------
// ImageStore
// ---------------------------------------------------------------------------

struct ImageStore::Impl {
    struct Entry {
        std::shared_ptr<ImageRequest> req;
        uint64_t lastUse = 0;
        size_t bytes = 0;  // counted once Ready
    };
    struct Job {
        std::shared_ptr<ImageRequest> req;
        ImageWork work;
    };

    mutable std::mutex mu;
    std::condition_variable workCv;   // a job queued, or stopping
    std::condition_variable idleCv;   // a job finished
    std::unordered_map<std::string, Entry> entries;
    std::deque<Job> queue;
    std::vector<std::thread> threads;
    size_t inFlight = 0;  // queued + running
    size_t budget = size_t(512) << 20;
    size_t cached = 0;
    uint64_t clock = 0;
    bool stopping = false;

    void evictLocked() {
        while (cached > budget) {
            auto victim = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (!it->second.req->settled()) continue;  // never a pending one
                if (victim == entries.end() || it->second.lastUse < victim->second.lastUse) victim = it;
            }
            if (victim == entries.end()) return;
            cached -= victim->second.bytes;
            entries.erase(victim);
        }
    }
};

ImageStore& ImageStore::instance() {
    static ImageStore store;
    return store;
}

ImageStore::ImageStore() : impl_(std::make_unique<Impl>()) {
    if (const char* mb = std::getenv("BRO_IMAGE_CACHE_MB")) {
        const long long v = std::atoll(mb);
        if (v >= 0) impl_->budget = static_cast<size_t>(v) << 20;
    }
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned n = std::clamp(hw / 2, 2u, 4u);
    for (unsigned i = 0; i < n; ++i) {
        impl_->threads.emplace_back([this] {
            for (;;) {
                Impl::Job job;
                {
                    std::unique_lock<std::mutex> lk(impl_->mu);
                    impl_->workCv.wait(lk, [this] { return impl_->stopping || !impl_->queue.empty(); });
                    if (impl_->stopping) return;
                    job = std::move(impl_->queue.front());
                    impl_->queue.pop_front();
                }
                run(job.req, std::move(job.work));
            }
        });
    }
}

ImageStore::~ImageStore() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->stopping = true;
        impl_->queue.clear();
    }
    impl_->workCv.notify_all();
    for (auto& t : impl_->threads)
        if (t.joinable()) t.join();
}

void ImageStore::run(const std::shared_ptr<ImageRequest>& req, ImageWork work) {
    auto img = std::make_shared<DecodedImage>();
    std::string err;
    bool ok = false;
    try {
        ok = work && work(*img, err);
    } catch (const std::exception& e) {
        err = e.what();
        ok = false;
    }
    if (ok && img->id == 0) img->id = nextPixelsId();
    {
        std::lock_guard<std::mutex> lk(req->mu_);
        if (ok) {
            req->image_ = std::move(img);
            req->state_.store(ImageRequest::State::Ready, std::memory_order_release);
        } else {
            req->error_ = err.empty() ? std::string("the image could not be decoded") : err;
            req->state_.store(ImageRequest::State::Failed, std::memory_order_release);
        }
    }
    req->cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!req->key_.empty()) {
            auto it = impl_->entries.find(req->key_);
            if (it != impl_->entries.end() && it->second.req == req && req->image_) {
                it->second.bytes = req->image_->bytes();
                impl_->cached += it->second.bytes;
                impl_->evictLocked();
            }
        }
        --impl_->inFlight;
        settled_.fetch_add(1, std::memory_order_acq_rel);
    }
    impl_->idleCv.notify_all();
    util::wakeMainLoop();
}

std::shared_ptr<ImageRequest> ImageStore::request(const std::string& key, ImageWork work) {
    std::shared_ptr<ImageRequest> req;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->entries.find(key);
        if (it != impl_->entries.end()) {
            it->second.lastUse = ++impl_->clock;
            return it->second.req;
        }
        req = std::make_shared<ImageRequest>();
        req->key_ = key;
        Impl::Entry e;
        e.req = req;
        e.lastUse = ++impl_->clock;
        impl_->entries.emplace(key, std::move(e));
        impl_->queue.push_back(Impl::Job{req, std::move(work)});
        ++impl_->inFlight;
    }
    impl_->workCv.notify_one();
    return req;
}

std::shared_ptr<ImageRequest> ImageStore::find(const std::string& key) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->entries.find(key);
    if (it == impl_->entries.end()) return nullptr;
    it->second.lastUse = ++impl_->clock;
    return it->second.req;
}

std::shared_ptr<ImageRequest> ImageStore::submit(ImageWork work) {
    auto req = std::make_shared<ImageRequest>();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->queue.push_back(Impl::Job{req, std::move(work)});
        ++impl_->inFlight;
    }
    impl_->workCv.notify_one();
    return req;
}

size_t ImageStore::pendingCount() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->inFlight;
}

bool ImageStore::waitIdle(double timeoutMs) {
    std::unique_lock<std::mutex> lk(impl_->mu);
    return impl_->idleCv.wait_for(lk, std::chrono::duration<double, std::milli>(timeoutMs),
                                  [this] { return impl_->inFlight == 0; });
}

size_t ImageStore::budgetBytes() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->budget;
}

void ImageStore::setBudgetBytes(size_t bytes) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->budget = bytes;
    impl_->evictLocked();
}

size_t ImageStore::cachedBytes() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->cached;
}

size_t ImageStore::cachedCount() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->entries.size();
}

}  // namespace bro::render

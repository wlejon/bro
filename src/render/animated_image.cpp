#include "render/animated_image.h"

#include "render/image_store.h"
#include "util/main_loop_wake.h"

#include "broimage/codec.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace bro::render {

namespace {

constexpr size_t kDefaultBudgetMB = 32;
constexpr size_t kMaxWindow = 8;
constexpr size_t kMinWindow = 2;

size_t budgetFromEnv() {
    if (const char* mb = std::getenv("BRO_IMAGE_ANIM_MB")) {
        const long long v = std::atoll(mb);
        if (v >= 0) return static_cast<size_t>(v) << 20;
    }
    return kDefaultBudgetMB << 20;
}

std::atomic<size_t>& budget() {
    static std::atomic<size_t> b{budgetFromEnv()};
    return b;
}

std::atomic<double> g_clock{0.0};

// The repaints painted animations asked for. Never destroyed: animations
// held by statics (the image store's cache) unregister during exit.
struct Scheduler {
    std::mutex mu;
    std::unordered_map<const AnimatedImage*, double> due;
    size_t live = 0;
    uint64_t paints = 0;
    uint64_t decoded = 0;
    uint64_t repaints = 0;
};

Scheduler& sched() {
    static Scheduler* s = new Scheduler;
    return *s;
}

// Frame pixels' ids: a range of their own (the store mints from bit 62, the
// renderer's other sources count up from 1).
uint64_t newFrameId() {
    static std::atomic<uint64_t> counter{1};
    return (uint64_t(1) << 61) | counter.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const DecodedImage> frameImage(int w, int h, std::vector<uint8_t>&& rgba) {
    auto img = std::make_shared<DecodedImage>();
    img->width = w;
    img->height = h;
    img->rgba = std::move(rgba);
    img->id = newFrameId();
    return img;
}

}  // namespace

size_t animationBudgetBytes() { return budget().load(std::memory_order_relaxed); }
void setAnimationBudgetBytes(size_t bytes) { budget().store(bytes, std::memory_order_relaxed); }

int effectiveFrameDelayMs(int storedMs) { return storedMs <= 10 ? 100 : storedMs; }

namespace {
std::function<double()>& clockSource() {
    static std::function<double()> source;
    return source;
}
}  // namespace

void setImageAnimationClockSource(std::function<double()> source) { clockSource() = std::move(source); }
void setImageAnimationClock(double nowMs) { g_clock.store(nowMs, std::memory_order_relaxed); }

double imageAnimationClock() {
    if (const auto& source = clockSource()) {
        const double now = source();
        g_clock.store(now, std::memory_order_relaxed);
        return now;
    }
    return g_clock.load(std::memory_order_relaxed);
}

double nextImageAnimationDueMs() {
    Scheduler& s = sched();
    std::lock_guard<std::mutex> lk(s.mu);
    double best = std::numeric_limits<double>::infinity();
    for (const auto& [a, t] : s.due) best = std::min(best, t);
    return best;
}

bool takeDueImageAnimations(double nowMs) {
    Scheduler& s = sched();
    std::lock_guard<std::mutex> lk(s.mu);
    bool any = false;
    for (auto it = s.due.begin(); it != s.due.end();) {
        if (it->second <= nowMs) {
            it = s.due.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    if (any) ++s.repaints;
    return any;
}

ImageAnimationStats imageAnimationStats() {
    Scheduler& s = sched();
    std::lock_guard<std::mutex> lk(s.mu);
    ImageAnimationStats st;
    st.live = s.live;
    st.scheduled = s.due.size();
    st.paints = s.paints;
    st.framesDecoded = s.decoded;
    st.repaints = s.repaints;
    return st;
}

// ---------------------------------------------------------------------------

struct AnimatedImage::Impl {
    std::mutex mu;
    std::condition_variable cv;
    std::unique_ptr<broimage::FrameDecoder> dec;
    int width = 0;
    int height = 0;
    int count = 0;      // frames per play
    int plays = 1;      // 0 = forever
    std::vector<double> prefix;  // start of each frame within a play; prefix[count] = its length

    // Every frame (cachesAll); [0] is null: the owning image.
    std::vector<std::shared_ptr<const DecodedImage>> frames;

    // Streaming: frames decoded ahead, in order, by sequence number
    // (play * count + index); a null image is frame 0.
    struct Slot {
        int64_t seq;
        std::shared_ptr<const DecodedImage> img;
    };
    std::deque<Slot> window;
    size_t windowMax = kMinWindow;
    int64_t decodedSeq = 1;  // the next sequence number the decoder produces
    bool decoding = false;
    bool decoderFailed = false;
    bool starved = false;    // painting a frame behind the one that is due

    double origin = std::numeric_limits<double>::quiet_NaN();
    int64_t shownSeq = 0;
    std::shared_ptr<const DecodedImage> shown;

    int64_t totalSeq() const {
        return plays > 0 ? int64_t(plays) * count : std::numeric_limits<int64_t>::max();
    }
    double startOf(int64_t seq) const {
        return origin + double(seq / count) * prefix[size_t(count)] + prefix[size_t(seq % count)];
    }

    struct Pos {
        int64_t seq = 0;
        double due = 0;
        bool finished = false;
    };
    Pos pos(double now) const {
        Pos p;
        const double length = prefix[size_t(count)];
        const double e = std::max(0.0, now - origin);
        if (plays > 0 && e >= double(plays) * length) {
            p.seq = int64_t(plays) * count - 1;
            p.finished = true;
            p.due = std::numeric_limits<double>::infinity();
            return p;
        }
        const double loop = std::floor(e / length);
        const double r = e - loop * length;
        int idx = int(std::upper_bound(prefix.begin(), prefix.end(), r) - prefix.begin()) - 1;
        idx = std::clamp(idx, 0, count - 1);
        p.seq = int64_t(loop) * count + idx;
        p.due = origin + loop * length + prefix[size_t(idx) + 1];
        return p;
    }

    size_t aheadOf(int64_t seq) const {
        size_t n = 0;
        for (const Slot& s : window)
            if (s.seq > seq) ++n;
        return n;
    }
};

std::shared_ptr<AnimatedImage> AnimatedImage::make(std::unique_ptr<broimage::FrameDecoder> dec) {
    if (!dec || dec->frame_count() < 2) return nullptr;
    std::shared_ptr<AnimatedImage> a(new AnimatedImage());
    a->impl_ = std::make_unique<Impl>();
    Impl& m = *a->impl_;
    m.width = dec->width();
    m.height = dec->height();
    m.count = dec->frame_count();
    m.plays = dec->loop_count();
    const std::vector<int> stored = dec->delays_ms();
    const size_t frameBytes = size_t(m.width) * size_t(m.height) * 4;
    const size_t limit = animationBudgetBytes();

    if (frameBytes > 0 && size_t(m.count - 1) * frameBytes <= limit) {
        a->cachesAll_ = true;
        m.frames.resize(size_t(m.count));
        broimage::AnimationFrame f;
        for (int i = 1; i < m.count; ++i) {
            if (!dec->next(f)) {
                m.count = i;  // a broken frame ends the animation there
                m.frames.resize(size_t(i));
                break;
            }
            m.frames[size_t(i)] = frameImage(m.width, m.height, std::move(f.rgba));
        }
        if (m.count < 2) return nullptr;
        a->budgetBytes_ = size_t(m.count - 1) * frameBytes;
        std::lock_guard<std::mutex> lk(sched().mu);
        sched().decoded += uint64_t(m.count - 1);
    } else {
        m.dec = std::move(dec);
        m.windowMax = std::clamp(frameBytes ? limit / frameBytes : kMinWindow, kMinWindow, kMaxWindow);
        // The window, plus the decoder's canvas and what it keeps for disposal.
        a->budgetBytes_ = (m.windowMax + 2) * frameBytes;
    }

    m.prefix.assign(size_t(m.count) + 1, 0.0);
    for (int i = 0; i < m.count; ++i)
        m.prefix[size_t(i) + 1] =
            m.prefix[size_t(i)] + effectiveFrameDelayMs(size_t(i) < stored.size() ? stored[size_t(i)] : 0);
    {
        std::lock_guard<std::mutex> lk(sched().mu);
        ++sched().live;
    }
    return a;
}

AnimatedImage::~AnimatedImage() {
    Scheduler& s = sched();
    std::lock_guard<std::mutex> lk(s.mu);
    s.due.erase(this);
    if (impl_ && s.live > 0) --s.live;
}

int AnimatedImage::frameCount() const { return impl_->count; }
int AnimatedImage::loopCount() const { return impl_->plays; }

int AnimatedImage::currentIndex() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return int(impl_->shownSeq % impl_->count);
}

void AnimatedImage::scheduleDecodeLocked() {
    Impl& m = *impl_;
    if (cachesAll_ || m.decoding || m.decoderFailed || !m.dec) return;
    if (m.decodedSeq >= m.totalSeq() || m.aheadOf(m.shownSeq) >= m.windowMax) return;
    m.decoding = true;
    std::weak_ptr<AnimatedImage> weak = weak_from_this();
    ImageStore::instance().post([weak] {
        if (auto self = weak.lock()) self->decodeAhead();
    });
}

void AnimatedImage::decodeAhead() {
    Impl& m = *impl_;
    for (;;) {
        int64_t seq = 0;
        {
            std::lock_guard<std::mutex> lk(m.mu);
            if (m.decoderFailed || m.decodedSeq >= m.totalSeq() || m.aheadOf(m.shownSeq) >= m.windowMax) {
                m.decoding = false;
                return;
            }
            seq = m.decodedSeq;
        }
        // The decoder is this task's alone while `decoding` is set.
        const int idx = int(seq % m.count);
        if (idx == 0) m.dec->rewind();
        broimage::AnimationFrame f;
        const bool ok = m.dec->next(f);
        std::shared_ptr<const DecodedImage> img;
        if (ok && idx != 0) img = frameImage(m.width, m.height, std::move(f.rgba));
        bool wake = false;
        {
            std::lock_guard<std::mutex> lk(m.mu);
            if (!ok) {
                // A broken frame: the animation rests on what decoded.
                m.decoderFailed = true;
                m.decoding = false;
            } else {
                m.window.push_back(Impl::Slot{seq, std::move(img)});
                m.decodedSeq = seq + 1;
            }
            if (m.starved) {
                m.starved = false;
                wake = true;
            }
            m.cv.notify_all();
            std::lock_guard<std::mutex> slk(sched().mu);
            if (ok) ++sched().decoded;
            // The painter was showing a stale frame: repaint now.
            if (wake) sched().due[this] = g_clock.load(std::memory_order_relaxed);
        }
        if (wake) util::wakeMainLoop();
        if (!ok) return;
    }
}

std::shared_ptr<const DecodedImage> AnimatedImage::frameAt(double nowMs, bool painted, bool wait) {
    Impl& m = *impl_;
    std::unique_lock<std::mutex> lk(m.mu);
    if (std::isnan(m.origin)) {
        if (!painted) return nullptr;  // never painted: the first frame
        m.origin = nowMs;
    }
    Impl::Pos p = m.pos(nowMs);
    std::shared_ptr<const DecodedImage> img;
    bool starved = false;
    if (cachesAll_) {
        m.shownSeq = p.seq;
        img = m.frames[size_t(p.seq % m.count)];
    } else {
        if (p.seq > m.shownSeq) {
            const bool have = m.decodedSeq > p.seq;
            // Too far behind to decode its way there (it was off screen, or
            // the decoder could not keep up): resume from the next frame.
            if (!have && !wait && p.seq >= m.decodedSeq + int64_t(m.windowMax)) {
                const int64_t target = m.shownSeq + 1;
                m.origin = nowMs - (m.startOf(target) - m.origin);
                p = m.pos(nowMs);
            }
            // Show the newest decoded frame up to the one that is due, and let
            // the ones before it go.
            auto advance = [&] {
                for (const Impl::Slot& s : m.window) {
                    if (s.seq > p.seq) break;
                    if (s.seq > m.shownSeq) {
                        m.shownSeq = s.seq;
                        m.shown = s.img;
                    }
                }
                while (!m.window.empty() && m.window.front().seq <= m.shownSeq) m.window.pop_front();
            };
            advance();
            // Headless: the frame that is due, decoded, rather than a stand-in.
            while (wait && m.shownSeq < p.seq && !m.decoderFailed) {
                scheduleDecodeLocked();
                if (!m.decoding) break;
                const int64_t before = m.decodedSeq;
                if (!m.cv.wait_for(lk, std::chrono::seconds(10), [&] {
                        return m.decodedSeq != before || m.decoderFailed || !m.decoding;
                    }))
                    break;
                advance();
            }
            starved = m.shownSeq < p.seq && !m.decoderFailed;
        }
        m.starved = starved;
        img = m.shown;
        scheduleDecodeLocked();
    }
    if (painted) {
        std::lock_guard<std::mutex> slk(sched().mu);
        ++sched().paints;
        if (p.finished || starved || (m.decoderFailed && m.shownSeq + 1 >= m.decodedSeq))
            sched().due.erase(this);
        else
            sched().due[this] = p.due;
    }
    return img;
}

}  // namespace bro::render

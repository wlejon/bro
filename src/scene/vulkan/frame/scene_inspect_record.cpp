#include "scene/vulkan/frame/scene_inspect_record.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define bro_fseek64 _fseeki64
#else
#define bro_fseek64 fseeko
#endif

namespace bro::scene::vk {

namespace {

constexpr char kMagic[8] = {'B', 'R', 'O', 'I', 'N', 'S', 'P', '2'};
// A frame's camera: three matrices of 16 floats, then the eye's 3 doubles.
constexpr size_t kCameraBytes = 3 * 16 * sizeof(float) + 3 * sizeof(double);

template <typename T>
void put(std::vector<uint8_t>& out, const T* v, size_t n) {
    const auto* b = reinterpret_cast<const uint8_t*>(v);
    out.insert(out.end(), b, b + n * sizeof(T));
}
template <typename T>
const uint8_t* take(const uint8_t* at, T* v, size_t n) {
    std::memcpy(v, at, n * sizeof(T));
    return at + n * sizeof(T);
}

void putCamera(std::vector<uint8_t>& out, const InspectCamera& c) {
    put(out, c.invProj.data, 16);
    put(out, c.relView.data, 16);
    put(out, c.relViewProj.data, 16);
    put(out, c.eye, 3);
}
void takeCamera(const uint8_t* at, InspectCamera& c) {
    at = take(at, c.invProj.data, 16);
    at = take(at, c.relView.data, 16);
    at = take(at, c.relViewProj.data, 16);
    take(at, c.eye, 3);
}

// A frame of the file: where its depth and luma start, its size.
struct FrameAt {
    int64_t offset = 0;   // its width's
    uint32_t w = 0, h = 0;
};

bool indexFrames(std::FILE* f, std::vector<FrameAt>& out, std::string& err) {
    char magic[8];
    uint32_t scale = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kMagic, 8) != 0 || std::fread(&scale, 4, 1, f) != 1) {
        err = "not an inspection recording";
        return false;
    }
    int64_t at = 12;
    for (;;) {
        uint32_t wh[2];
        if (bro_fseek64(f, at, SEEK_SET) != 0 || std::fread(wh, 4, 2, f) != 2) break;
        const int64_t n = static_cast<int64_t>(wh[0]) * wh[1];
        const int64_t size = 8 + static_cast<int64_t>(kCameraBytes) + n * 5;
        out.push_back({at, wh[0], wh[1]});
        at += size;
    }
    // (A frame cut short, the recording stopped while it was written, is not
    // one.)
    if (!out.empty()) {
        bro_fseek64(f, 0, SEEK_END);
        const FrameAt& last = out.back();
        int64_t end = 0;
#ifdef _WIN32
        end = _ftelli64(f);
#else
        end = ftello(f);
#endif
        if (last.offset + 8 + static_cast<int64_t>(kCameraBytes) + static_cast<int64_t>(last.w) * last.h * 5 > end)
            out.pop_back();
    }
    return true;
}

struct FrameData {
    InspectCamera cam;
    std::vector<float> depth;
    std::vector<uint8_t> luma;
};

bool readFrame(std::FILE* f, const FrameAt& at, FrameData& out) {
    const size_t n = static_cast<size_t>(at.w) * at.h;
    std::vector<uint8_t> cam(kCameraBytes);
    out.depth.resize(n);
    out.luma.resize(n);
    if (bro_fseek64(f, at.offset + 8, SEEK_SET) != 0 || std::fread(cam.data(), 1, kCameraBytes, f) != kCameraBytes ||
        std::fread(out.depth.data(), sizeof(float), n, f) != n || std::fread(out.luma.data(), 1, n, f) != n)
        return false;
    takeCamera(cam.data(), out.cam);
    return true;
}

}  // namespace

bool InspectRecorder::open(const std::string& path, int scale, std::string& err) {
    close();
    scale_ = std::clamp(scale, 1, 8);
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        err = "cannot write " + path;
        return false;
    }
    const uint32_t s = static_cast<uint32_t>(scale_);
    std::fwrite(kMagic, 1, 8, file_);
    std::fwrite(&s, 4, 1, file_);
    frames_ = 0;
    closing_ = false;
    thread_ = std::thread([this] { writer(); });
    return true;
}

void InspectRecorder::add(const InspectCamera& cam, uint32_t width, uint32_t height, const float* depth,
                          const uint8_t* rgba) {
    if (!file_) return;
    const uint32_t s = static_cast<uint32_t>(scale_);
    const uint32_t w = width / s, h = height / s;
    if (w == 0 || h == 0) return;
    std::vector<uint8_t> out;
    out.reserve(8 + kCameraBytes + static_cast<size_t>(w) * h * 5);
    const uint32_t wh[2] = {w, h};
    put(out, wh, 2);
    putCamera(out, cam);
    // The depth of each block's middle pixel (a mean of depths would put
    // points between a silhouette's two sides), the luma its mean.
    const size_t depthAt = out.size();
    out.resize(depthAt + static_cast<size_t>(w) * h * 5);
    auto* d = reinterpret_cast<float*>(out.data() + depthAt);
    uint8_t* l = out.data() + depthAt + static_cast<size_t>(w) * h * sizeof(float);
    const uint32_t mid = s / 2;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t o = static_cast<size_t>(y) * w + x;
            std::memcpy(d + o, depth + static_cast<size_t>(y * s + mid) * width + (x * s + mid), sizeof(float));
            uint32_t sum = 0;
            for (uint32_t by = 0; by < s; ++by) {
                const uint8_t* row = rgba + (static_cast<size_t>(y * s + by) * width + x * s) * 4;
                for (uint32_t bx = 0; bx < s; ++bx) sum += ViewInspector::lumaOf(row + bx * 4);
            }
            l[o] = static_cast<uint8_t>((sum + s * s / 2) / (s * s));
        }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return queue_.size() < kQueued; });
    queue_.push_back(std::move(out));
    ++frames_;
    cv_.notify_all();
}

void InspectRecorder::writer() {
    for (;;) {
        std::vector<uint8_t> next;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            if (queue_.empty()) return;
            next = std::move(queue_.front());
            queue_.pop_front();
            cv_.notify_all();
        }
        std::fwrite(next.data(), 1, next.size(), file_);
    }
}

void InspectRecorder::close() {
    if (!file_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::fclose(file_);
    file_ = nullptr;
}

bool inspectRecording(const std::string& path, ViewInspector::Image image, const std::vector<uint32_t>& imageFrames,
                      std::vector<double>& stats, std::vector<std::vector<uint8_t>>& images, uint32_t& width,
                      uint32_t& height, std::string& err) {
    stats.clear();
    images.clear();
    width = height = 0;
    std::vector<FrameAt> frames;
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            err = "cannot read " + path;
            return false;
        }
        const bool ok = indexFrames(f, frames, err);
        std::fclose(f);
        if (!ok) return false;
    }
    if (frames.empty()) return true;
    width = frames.front().w;
    height = frames.front().h;
    const size_t count = frames.size();
    stats.assign(count * ViewInspector::kStatCount, 0.0);
    images.resize(imageFrames.size());
    // The frames shared out in runs, each run's inspector starting from the
    // frame before it (built, not counted), so every frame is compared with
    // the one kept before it whichever run it is in.
    const size_t threads = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 32);
    const size_t run = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    std::vector<std::string> errs(threads);
    for (size_t t = 0; t < threads; ++t) {
        const size_t from = t * run, to = std::min(count, from + run);
        if (from >= to) break;
        pool.emplace_back([&, t, from, to] {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) {
                errs[t] = "cannot read " + path;
                return;
            }
            ViewInspector inspector;
            FrameData data;
            std::vector<double> s;
            std::vector<uint8_t> px;
            for (size_t i = from == 0 ? 0 : from - 1; i < to; ++i) {
                if (!readFrame(f, frames[i], data)) {
                    errs[t] = "a frame of " + path + " did not read";
                    break;
                }
                const auto wanted = std::find(imageFrames.begin(), imageFrames.end(), static_cast<uint32_t>(i));
                const bool counted = i >= from;
                const ViewInspector::Image im = counted && wanted != imageFrames.end() ? image : ViewInspector::Image::None;
                inspector.inspect(data.cam, frames[i].w, frames[i].h, data.depth.data(), data.luma.data(), im, s, px);
                if (!counted) continue;
                std::copy(s.begin(), s.end(), stats.begin() + static_cast<ptrdiff_t>(i * ViewInspector::kStatCount));
                if (im != ViewInspector::Image::None) images[static_cast<size_t>(wanted - imageFrames.begin())] = px;
            }
            std::fclose(f);
        });
    }
    for (std::thread& th : pool) th.join();
    for (const std::string& e : errs)
        if (!e.empty()) {
            err = e;
            return false;
        }
    return true;
}

}  // namespace bro::scene::vk

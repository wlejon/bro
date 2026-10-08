#include "render/skia_persistent_cache.h"
#include "render/vulkan_pipeline_cache.h"
#include "util/log.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace bro::render {

namespace {

// The file: a header, then (key size, data size, key, data) records. Skia's
// keys do not cover how a program lays out its uniforms, so the version in
// the header changes whenever bro changes that: 02 = no push constants (see
// skiaPhysicalDeviceProperties in skia_gpu.cpp); a 01 file's shaders read
// their uniforms from push constants Skia no longer sets.
constexpr char kMagic[8] = {'B', 'R', 'O', 'S', 'K', 'C', '0', '2'};
// A cache past this size is not written back; the next launch rebuilds what
// it uses, which bounds the file without an LRU.
constexpr size_t kMaxBytes = 64u << 20;

void put32(std::vector<char>& out, uint32_t v) {
    const char* p = reinterpret_cast<const char*>(&v);
    out.insert(out.end(), p, p + sizeof(v));
}

} // namespace

SkiaPersistentCache::SkiaPersistentCache(std::string path) : path_(std::move(path)) {
    if (path_.empty()) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    const std::vector<char> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (file.size() < sizeof(kMagic) || std::memcmp(file.data(), kMagic, sizeof(kMagic)) != 0) {
        LOG_INFO("SkiaGpu: ignoring the shader cache %s (another format or version)", path_.c_str());
        return;
    }
    size_t at = sizeof(kMagic);
    while (at + 8 <= file.size()) {
        uint32_t keySize = 0, dataSize = 0;
        std::memcpy(&keySize, file.data() + at, 4);
        std::memcpy(&dataSize, file.data() + at + 4, 4);
        at += 8;
        if (file.size() - at < static_cast<size_t>(keySize) + dataSize) break;  // truncated: keep what parsed
        std::string key(file.data() + at, keySize);
        entries_[std::move(key)] = SkData::MakeWithCopy(file.data() + at + keySize, dataSize);
        bytes_ += keySize + dataSize;
        at += static_cast<size_t>(keySize) + dataSize;
    }
    LOG_INFO("SkiaGpu: shader cache %s (%zu entries)", path_.c_str(), entries_.size());
}

sk_sp<SkData> SkiaPersistentCache::load(const SkData& key) {
    auto it = entries_.find(std::string(static_cast<const char*>(key.data()), key.size()));
    return it == entries_.end() ? nullptr : it->second;
}

void SkiaPersistentCache::store(const SkData& key, const SkData& data, const SkString&) {
    std::string k(static_cast<const char*>(key.data()), key.size());
    sk_sp<SkData>& slot = entries_[std::move(k)];
    if (slot) {
        if (slot->equals(&data)) return;
        bytes_ -= slot->size();
    } else {
        bytes_ += key.size();
    }
    slot = SkData::MakeWithCopy(data.data(), data.size());
    bytes_ += data.size();
    dirty_ = true;
}

void SkiaPersistentCache::save() {
    if (path_.empty() || !dirty_ || bytes_ > kMaxBytes) return;
    std::vector<char> out(kMagic, kMagic + sizeof(kMagic));
    out.reserve(sizeof(kMagic) + bytes_ + entries_.size() * 8);
    for (const auto& [key, data] : entries_) {
        put32(out, static_cast<uint32_t>(key.size()));
        put32(out, static_cast<uint32_t>(data->size()));
        out.insert(out.end(), key.begin(), key.end());
        const char* d = static_cast<const char*>(data->data());
        out.insert(out.end(), d, d + data->size());
    }
    if (writeCacheFile(path_, out.data(), out.size())) dirty_ = false;
    else LOG_WARN("SkiaGpu: could not write the shader cache to %s", path_.c_str());
}

} // namespace bro::render

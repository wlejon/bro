#pragma once

#include <include/core/SkData.h>
#include <include/core/SkRefCnt.h>
#include <include/gpu/ganesh/GrContextOptions.h>

#include <map>
#include <string>

namespace bro::render {

/// Skia's persistent cache, kept in one file beside the device's
/// VkPipelineCache (VulkanPipelineCache::path() with a `.skia` suffix): the
/// SPIR-V of every shader Ganesh compiled and Ganesh's own VkPipelineCache
/// blob, so a warm launch skips both the SkSL compile and the driver's
/// pipeline build for the UI. Read whole at construction; written back by
/// save() when something new was stored. Skia validates its pipeline-cache
/// blob against the device itself, and the file is per device anyway.
///
/// Not locked: Skia calls it only from inside its context, and every Skia call
/// in bro runs under SkiaGpu's lock.
class SkiaPersistentCache final : public GrContextOptions::PersistentCache {
public:
    /// `path` empty: a cache that lives for the run only.
    explicit SkiaPersistentCache(std::string path);

    sk_sp<SkData> load(const SkData& key) override;
    void store(const SkData& key, const SkData& data, const SkString& description) override;

    /// Write the file if anything was stored since it was read.
    void save();

private:
    std::string path_;
    std::map<std::string, sk_sp<SkData>> entries_;
    size_t bytes_ = 0;
    bool dirty_ = false;
};

} // namespace bro::render

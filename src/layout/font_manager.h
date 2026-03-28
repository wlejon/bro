#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace bro::render { class Renderer; }

namespace bro::layout {

struct FontMetrics {
    float height = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float x_height = 0.0f;
};

class FontManager {
public:
    FontManager() = default;
    ~FontManager() = default;

    /// Create a font (or return cached handle). Metrics are stored internally.
    uint64_t createFont(render::Renderer* renderer,
                        const std::string& family,
                        float size, int weight, bool italic);

    /// Delete a font and remove it from cache.
    void deleteFont(render::Renderer* renderer, uint64_t handle);

    /// Retrieve stored metrics for a previously-created font.
    FontMetrics getMetrics(uint64_t handle) const;

private:
    struct CacheKey {
        std::string family;
        float size;
        int weight;
        bool italic;

        bool operator<(const CacheKey& o) const {
            if (family != o.family) return family < o.family;
            if (size != o.size) return size < o.size;
            if (weight != o.weight) return weight < o.weight;
            return italic < o.italic;
        }
    };

    std::map<CacheKey, uint64_t> cache_;
    std::map<uint64_t, FontMetrics> metrics_;
};

} // namespace bro::layout

#include "layout/font_manager.h"
#include "render/renderer.h"

namespace bro::layout {

uint64_t FontManager::createFont(render::Renderer* renderer,
                                  const std::string& family,
                                  float size, int weight, bool italic) {
    CacheKey key{family, size, weight, italic};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    uint64_t handle = renderer->createFont(family, size, weight, italic);

    // Measure a representative string to approximate metrics.
    auto tm = renderer->measureText("Xg", handle);
    FontMetrics fm;
    fm.ascent = tm.ascent > 0 ? tm.ascent : tm.height * 0.8f;
    fm.descent = tm.descent > 0 ? tm.descent : tm.height * 0.2f;
    fm.height = fm.ascent + fm.descent;
    // x_height: measure 'x' specifically
    auto xm = renderer->measureText("x", handle);
    fm.x_height = xm.height * 0.7f;

    cache_[key] = handle;
    metrics_[handle] = fm;
    return handle;
}

void FontManager::deleteFont(render::Renderer* renderer, uint64_t handle) {
    renderer->deleteFont(handle);

    // Remove from metrics
    metrics_.erase(handle);

    // Remove from cache
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->second == handle) {
            cache_.erase(it);
            break;
        }
    }
}

FontMetrics FontManager::getMetrics(uint64_t handle) const {
    auto it = metrics_.find(handle);
    if (it != metrics_.end()) {
        return it->second;
    }
    return {};
}

} // namespace bro::layout

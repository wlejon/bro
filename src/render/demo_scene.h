#pragma once

#include "render/scene_layer.h"
#include "render/renderer.h"
#include <cmath>

namespace bro::render {

/// Animated demo scene rendered via the Renderer abstraction.
class DemoScene final : public SceneLayer {
public:
    void onInit(Renderer& /*renderer*/, int /*w*/, int /*h*/) override {}
    void onResize(int /*w*/, int /*h*/) override {}
    void onCleanup() override {}

    void onRender(Renderer& renderer, int w, int h, double /*deltaTimeMs*/) override {
        time_ += 0.016;

        // Animated dark gradient background bands
        int bands = 16;
        float bandH = static_cast<float>(h) / bands;
        for (int i = 0; i < bands; ++i) {
            float t = static_cast<float>(i) / bands;
            float phase = static_cast<float>(time_) * 0.5f + t * 3.14159f;

            uint8_t r = static_cast<uint8_t>(25 + 25 * std::sin(phase));
            uint8_t g = static_cast<uint8_t>(25 + 25 * std::sin(phase + 2.094f));
            uint8_t b = static_cast<uint8_t>(40 + 35 * std::sin(phase + 4.189f));

            renderer.fillRect(0.0f, i * bandH,
                              static_cast<float>(w), bandH + 1.0f,
                              {r, g, b, 255});
        }

        // Floating orbs
        for (int i = 0; i < 8; ++i) {
            float cx = w * 0.5f + w * 0.35f * static_cast<float>(std::cos(time_ * (0.2 + i * 0.08) + i * 1.5));
            float cy = h * 0.5f + h * 0.35f * static_cast<float>(std::sin(time_ * (0.15 + i * 0.1) + i * 2.3));
            float size = 15.0f + 20.0f * static_cast<float>(std::sin(time_ * 0.6 + i));

            uint8_t r = static_cast<uint8_t>(50 + 50 * std::sin(time_ * 0.4 + i));
            uint8_t g = static_cast<uint8_t>(70 + 50 * std::sin(time_ * 0.3 + i + 1));
            uint8_t b = static_cast<uint8_t>(120 + 50 * std::sin(time_ * 0.2 + i + 2));

            renderer.fillRect(cx - size, cy - size, size * 2, size * 2,
                              {r, g, b, 140});
        }

        // Grid lines
        for (int gx = 0; gx < w; gx += 80) {
            float offset = static_cast<float>(std::fmod(time_ * 20.0, 80.0));
            float x = static_cast<float>(gx) + offset;
            if (x < w) {
                renderer.drawLine(x, 0.0f, x, static_cast<float>(h),
                                  {60, 60, 80, 60}, 1.0f);
            }
        }
        for (int gy = 0; gy < h; gy += 80) {
            float offset = static_cast<float>(std::fmod(time_ * 15.0, 80.0));
            float y = static_cast<float>(gy) + offset;
            if (y < h) {
                renderer.drawLine(0.0f, y, static_cast<float>(w), y,
                                  {60, 60, 80, 60}, 1.0f);
            }
        }
    }

private:
    double time_ = 0.0;
};

} // namespace bro::render

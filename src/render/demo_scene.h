#pragma once

#include "render/scene_layer.h"
#include <SDL3/SDL.h>
#include <cmath>

namespace bro::render {

/// Animated demo scene rendered via SDL GPU-accelerated primitives.
class DemoScene final : public SceneLayer {
public:
    void onInit(SDL_Renderer* /*sdlRenderer*/, int /*w*/, int /*h*/) override {}
    void onResize(int /*w*/, int /*h*/) override {}
    void onCleanup() override {}

    void onRender(SDL_Renderer* r, int w, int h, double deltaTimeMs) override {
        time_ += deltaTimeMs / 1000.0;

        float wf = static_cast<float>(w);
        float hf = static_cast<float>(h);

        // Animated dark gradient background bands
        int bands = 16;
        float bandH = hf / bands;
        for (int i = 0; i < bands; ++i) {
            float t = static_cast<float>(i) / bands;
            float phase = static_cast<float>(time_) * 0.5f + t * 3.14159f;

            uint8_t cr = static_cast<uint8_t>(25 + 25 * std::sin(phase));
            uint8_t cg = static_cast<uint8_t>(25 + 25 * std::sin(phase + 2.094f));
            uint8_t cb = static_cast<uint8_t>(40 + 35 * std::sin(phase + 4.189f));

            SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
            SDL_FRect rect = {0.0f, i * bandH, wf, bandH + 1.0f};
            SDL_RenderFillRect(r, &rect);
        }

        // Floating orbs
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        for (int i = 0; i < 8; ++i) {
            float cx = wf * 0.5f + wf * 0.35f * static_cast<float>(std::cos(time_ * (0.2 + i * 0.08) + i * 1.5));
            float cy = hf * 0.5f + hf * 0.35f * static_cast<float>(std::sin(time_ * (0.15 + i * 0.1) + i * 2.3));
            float size = 15.0f + 20.0f * static_cast<float>(std::sin(time_ * 0.6 + i));

            uint8_t cr = static_cast<uint8_t>(50 + 50 * std::sin(time_ * 0.4 + i));
            uint8_t cg = static_cast<uint8_t>(70 + 50 * std::sin(time_ * 0.3 + i + 1));
            uint8_t cb = static_cast<uint8_t>(120 + 50 * std::sin(time_ * 0.2 + i + 2));

            SDL_SetRenderDrawColor(r, cr, cg, cb, 140);
            SDL_FRect rect = {cx - size, cy - size, size * 2, size * 2};
            SDL_RenderFillRect(r, &rect);
        }

        // Grid lines
        for (int gx = 0; gx < w; gx += 80) {
            float offset = static_cast<float>(std::fmod(time_ * 20.0, 80.0));
            float x = static_cast<float>(gx) + offset;
            if (x < wf) {
                SDL_SetRenderDrawColor(r, 60, 60, 80, 60);
                SDL_RenderLine(r, x, 0.0f, x, hf);
            }
        }
        for (int gy = 0; gy < h; gy += 80) {
            float offset = static_cast<float>(std::fmod(time_ * 15.0, 80.0));
            float y = static_cast<float>(gy) + offset;
            if (y < hf) {
                SDL_SetRenderDrawColor(r, 60, 60, 80, 60);
                SDL_RenderLine(r, 0.0f, y, wf, y);
            }
        }
    }

private:
    double time_ = 0.0;
};

} // namespace bro::render

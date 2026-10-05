#pragma once

// TerrainManager internals shared by its translation units: the FastNoise2
// node tree behind noise_ (built in configure(), sampled by the heightmap
// generator).

#include "scene/terrain_manager.h"

#include <FastNoise/FastNoise.h>

#include <algorithm>

namespace bro::scene {

// -------------------------------------------------------------------------
// NoiseState — wraps FastNoise2 node tree
// -------------------------------------------------------------------------

struct TerrainManager::NoiseState {
    FastNoise::SmartNode<> node;
    FastNoise::SmartNode<> continentNode;  // large-scale amplitude modulation
    FastNoise::SmartNode<> mountainNode;   // enormous mountain pass
    float maxAmplitude = 1.0f;
    float mountainMaxAmplitude = 1.0f;

    void build(const TerrainConfig& cfg) {
        auto simplex = FastNoise::New<FastNoise::Simplex>();
        auto fbm = FastNoise::New<FastNoise::FractalFBm>();
        fbm->SetSource(simplex);
        fbm->SetOctaveCount(cfg.noiseOctaves);
        fbm->SetGain(cfg.noiseGain);
        fbm->SetLacunarity(cfg.noiseLacunarity);
        node = fbm;

        float amp = 0.0f;
        float g = 1.0f;
        for (int i = 0; i < cfg.noiseOctaves; i++) {
            amp += g;
            g *= cfg.noiseGain;
        }
        maxAmplitude = std::max(amp, 1.0f);

        // Continental noise: low-frequency simplex for regional variation
        if (cfg.continentFrequency > 0.0f) {
            auto csimplex = FastNoise::New<FastNoise::Simplex>();
            auto cfbm = FastNoise::New<FastNoise::FractalFBm>();
            cfbm->SetSource(csimplex);
            cfbm->SetOctaveCount(3);
            cfbm->SetGain(0.5f);
            cfbm->SetLacunarity(2.0f);
            continentNode = cfbm;
        } else {
            continentNode = nullptr;
        }

        // Mountain pass: enormous low-frequency features
        if (cfg.mountainFrequency > 0.0f) {
            auto msimplex = FastNoise::New<FastNoise::Simplex>();
            auto mfbm = FastNoise::New<FastNoise::FractalFBm>();
            mfbm->SetSource(msimplex);
            mfbm->SetOctaveCount(cfg.mountainOctaves);
            mfbm->SetGain(0.5f);
            mfbm->SetLacunarity(2.0f);
            mountainNode = mfbm;

            float mamp = 0.0f;
            float mg = 1.0f;
            for (int i = 0; i < cfg.mountainOctaves; i++) {
                mamp += mg;
                mg *= 0.5f;
            }
            mountainMaxAmplitude = std::max(mamp, 1.0f);
        } else {
            mountainNode = nullptr;
        }
    }
};

} // namespace bro::scene

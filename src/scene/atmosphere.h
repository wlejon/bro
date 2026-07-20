#pragma once

namespace bro::scene {

// Parameters for the analytic atmosphere (src/scene/shaders/atmosphere.glsl).
//
// Defaults are Earth: Rayleigh and Mie coefficients from the standard fits at
// 680/550/440 nm, an 8 km Rayleigh scale height and 1.2 km for haze. They are
// exposed rather than baked because the point of the model is that a different
// planet is a parameter change, not a different shader.
struct AtmosphereParams {
    bool  enabled = false;

    // Unit vector TOWARDS the sun. Normalised when uploaded.
    float sunDir[3]   = {0.0f, 0.35f, -0.94f};
    // Radiance of the solar disk at the top of the atmosphere. Scene units are
    // arbitrary, so this is really "how bright is the sky" and is meant to be
    // set alongside the directional light that casts the matching shadows.
    float sunColor[3] = {20.0f, 20.0f, 20.0f};

    float planetRadius = 6371000.0f;   // metres
    float thickness    = 100000.0f;    // metres of air above the surface

    float betaR[3]     = {5.802e-6f, 13.558e-6f, 33.1e-6f};  // per metre
    float betaM        = 3.996e-6f;
    float mieG         = 0.76f;
    float scaleHeightR = 8000.0f;
    float scaleHeightM = 1200.0f;

    // World Y that sits on the planet's surface. Terrain built around y = 0
    // wants 0; a world whose ground sits at 1200 m wants 1200, or the viewer
    // starts a kilometre of extra air deep.
    float seaLevel = 0.0f;

    float sunAngularRadius = 0.00465f;   // radians; the real sun
    float sunDiskIntensity = 25.0f;
};

} // namespace bro::scene

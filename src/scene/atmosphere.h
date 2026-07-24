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
    // False means sunColor TRACKS the scene's brightest directional light
    // (colour * intensity), which is what keeps inscatter and lit ground on
    // one scale. Set only when an app passes sunColor explicitly.
    bool  sunColorExplicit = false;

    float planetRadius = 6371000.0f;   // metres
    float thickness    = 100000.0f;    // metres of air above the surface

    float betaR[3]     = {5.802e-6f, 13.558e-6f, 33.1e-6f};  // per metre
    float betaM        = 3.996e-6f;
    float mieG         = 0.76f;
    float scaleHeightR = 8000.0f;
    float scaleHeightM = 1200.0f;

    // World Y that sits on the planet's surface. Terrain built around y = 0
    // wants 0; a world whose ground sits at 1200 m wants 1200, or the viewer
    // starts a kilometre of extra air deep. Ignored in spherical mode, where
    // the radius already says where the surface is.
    float seaLevel = 0.0f;

    // FLAT (default) or SPHERICAL, which is a statement about the SCENE, not
    // about the model — the scattering integral has always been spherical.
    //
    // Flat: the world is a height field whose up is +Y everywhere, so the
    // planet is placed directly beneath the viewer. Anchoring it anywhere fixed
    // would tilt the air's local vertical away from the terrain's as the viewer
    // travelled, drawing a second sloping horizon across the sky.
    //
    // Spherical: the scene contains an actual globe centred at `center`, and
    // the air must be centred on the same point or the atmosphere slides off
    // the planet as soon as the camera leaves the pole facing it. This is the
    // mode for a body viewed from orbit.
    bool  spherical  = false;
    float center[3]  = {0.0f, 0.0f, 0.0f};   // world position of the planet's centre

    // Strength of the isotropic multiple-scattering fill (0 = single scatter
    // only, the historical behaviour). Lets sunColor drop to a physical value
    // — which keeps aerial perspective over ground crisp — while the sky stays
    // bright and blue from inside the atmosphere. See atmosphere.glsl.
    float multiScatter = 0.0f;

    float sunAngularRadius = 0.00465f;   // radians; the real sun
    float sunDiskIntensity = 25.0f;
};

// Parameters for the additive starfield (src/scene/shaders/starfield.frag).
// Drawn over whichever sky is active, so the same field reads as invisible in a
// bright day sky and as stars against the near-black sky above the atmosphere.
struct StarfieldParams {
    bool  enabled   = false;
    float intensity = 1.0f;   // overall brightness
    float density   = 1.0f;   // ~0..2, relative fraction of cells that hold a star
    float rotation  = 0.0f;   // radians; slowly turns the celestial sphere about Y
};

} // namespace bro::scene

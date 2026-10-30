#pragma once

// Exposure in EV (docs/lighting-api.js, ExposureConfig): the scale the HDR
// frame is multiplied by before the tone curve, as an EV100 (the camera's
// exposure value at ISO 100), so scene units only need to be self-consistent.
//
//   scale = 1 / (1.2 * 2^(EV100 - compensation))      (Lagarde & de Rousiers 2014)
//
// Fixed: EV100 is the setting. The default is -log2(1.2), a scale of exactly
// 1, so a scene authored in display units passes to the tone curve as it is.
// Auto: EV100 is metered from the frame's luminance on the GPU (a histogram
// of log2 luminance; L the log-average of the part between lowPercent and
// highPercent of the pixels, and the EV the one whose scale puts L at middle
// grey, 0.18: EV100 = log2(L / (1.2 * 0.18))) and the applied EV follows it
// exponentially, at speedUp per second toward a brighter scene and speedDown
// toward a darker one: no overshoot, no oscillation, and the frame never
// waits for it (scene/vulkan/post/scene_exposure.h).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bro::scene {

struct ExposureSettings {
    enum class Mode : uint8_t { Fixed, Auto };
    static constexpr double kUnitScaleEv = -0.26303440583379378;   // -log2(1.2): a scale of 1

    Mode mode = Mode::Fixed;
    float ev100 = static_cast<float>(kUnitScaleEv);   // Fixed: the exposure
    float compensation = 0.0f;   // EV added to the exposure (+1: twice as bright)
    float speedUp = 3.0f;        // 1/s toward a brighter scene (the metered EV rising)
    float speedDown = 1.0f;      // 1/s toward a darker one
    float minEv = -6.0f;         // the metered EV's clamp
    float maxEv = 20.0f;
    // The meter reads most of the frame, so an open sky over a sunlit field
    // weighs as much as the field and the field lands near middle grey.
    float lowPercent = 0.1f;     // the share of darkest pixels the meter ignores
    float highPercent = 0.9f;    // the share below which it reads (the brightest 10% ignored)
    // Auto: what the meter reads. Reflected: the frame's luminance at middle
    // grey, which lifts a dark scene (forest, a planet's ocean) to grey and
    // washes it out. Incident: the light arriving, as a hand-held incident
    // meter at the eye reads it: the sun through the air there (the
    // atmosphere's transmittance; with the atmosphere off, the reflected
    // meter) and the sky's light on a level surface, at the EV the reflected
    // meter gives a grey card in it. Below a grey card's 3 cd/m^2 each stop
    // less light is half a stop (the eye's threshold goes as the square
    // root of the light there, de Vries-Rose), so dusk and night read dim.
    // It is kept within kIncidentBrighter stops brighter and kIncidentDarker
    // darker than the reflected reading, so shade and night are still seen.
    // The scene's volumes dim the sun by the share they pass from the eye
    // toward it and the sky by the share straight up, and add their own
    // light from straight up, as an overcast sky's (volume.frag,
    // exposure.comp), so under a deck it reads the deck's light.
    enum class Meter : uint8_t { Reflected, Incident };
    Meter meter = Meter::Reflected;
};

namespace exposure {

/// log2 luminance the histogram covers, and its bins.
constexpr float kMinLog2 = -12.0f;
constexpr float kMaxLog2 = 22.0f;
constexpr int kBins = 128;

/// The scale an EV100 gives (computed in double, so the default is exactly 1).
inline float scaleForEv(double ev100) { return static_cast<float>(1.0 / (1.2 * std::exp2(ev100))); }
/// The incident meter's EV for an illuminance E (lux): the one the reflected
/// meter gives a grey card (0.18, Lambertian, luminance 0.18 E / pi) in that
/// light, log2(E / (1.2 pi)), so the two meters agree on a grey world; and
/// its reach about the reflected reading.
inline double incidentEvOffset() { return -std::log2(1.2 * 3.14159265358979323846); }
constexpr float kIncidentBrighter = 1.0f;
constexpr float kIncidentDarker = 3.0f;
/// The key the meter puts the log-average luminance on.
constexpr double kMiddleGrey = 0.18;
/// The EV100 a log-average luminance meters to: the one whose scale puts it at middle grey.
inline double evForLuminance(double luminance) { return std::log2(std::max(luminance, 1e-9) / (1.2 * kMiddleGrey)); }
/// One frame of adaptation: `current` toward `target` over `dt` seconds.
inline double adapt(double current, double target, double dt, double speedUp, double speedDown) {
    const double speed = target > current ? speedUp : speedDown;
    return current + (target - current) * (1.0 - std::exp(-std::max(dt, 0.0) * std::max(speed, 0.0)));
}

}  // namespace exposure

}  // namespace bro::scene

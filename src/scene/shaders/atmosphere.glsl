// Analytic atmospheric scattering — shared by the sky pass and by the aerial
// perspective applied to scene geometry.
//
// This exists to replace two things that were never going to look like air:
// a fixed-colour exponential fog, and a flat HDR ambient term. Fog with one
// colour cannot be blue toward the zenith and warm toward the sun, so distance
// reads as a grey wash rather than as depth; and a flat ambient makes lit snow
// blow out to white because the sky it is supposedly reflecting is a constant.
//
// Single scattering, integrated along the view ray, in SPHERICAL geometry.
// Spherical rather than a flat slab because the whole point is that this stays
// valid from a metre above the ground to orbit: a slab has no horizon, so the
// planet's edge never curves and the sky above never thins out.
//
// The same function serves the sky and the haze on a mountain ten kilometres
// away, so the two cannot disagree — a distant ridge fades into exactly the
// colour the sky has behind it, which is what actually sells distance.

uniform vec3  uAtmSunDir;        // unit vector TOWARDS the sun
uniform vec3  uAtmSunColor;      // solar irradiance at the top of the atmosphere
uniform float uAtmPlanetRadius;  // metres
uniform float uAtmThickness;     // metres of atmosphere above the surface
uniform vec3  uAtmBetaR;         // Rayleigh scattering at sea level, per metre
uniform float uAtmBetaM;         // Mie scattering at sea level, per metre
uniform float uAtmMieG;          // Mie asymmetry, ~0.76 for haze
uniform float uAtmScaleHeightR;  // Rayleigh density scale height, metres
uniform float uAtmScaleHeightM;  // Mie density scale height, metres
uniform float uAtmSeaLevel;      // world Y that counts as the planet's surface

// Mie absorbs as well as scatters; the usual approximation is that extinction
// is a little larger than scattering. Rayleigh does not absorb.
const float ATM_MIE_EXTINCTION = 1.1;

// View position relative to the planet centre.
//
// The horizontal components are deliberately DROPPED: the planet is placed
// directly beneath the viewer rather than beneath the world origin. The
// sphere is here to give the air a horizon and a thickness that falls off with
// height — but the surface it has to agree with is a FLAT height field whose
// up is world +Y everywhere. Anchoring the sphere at the origin instead tilts
// the atmosphere's local vertical by atan(d / R) once the viewer is d metres
// away from it, which draws a second, sloping horizon across the sky that
// pulls further from the terrain's the further you travel — about 1.4 degrees
// at 150 km out, and unbounded beyond that.
//
// Everything downstream is a function of |ro| and of the ray direction, so
// this costs nothing and keeps sky, aerial perspective and the CPU irradiance
// integrator (scene/atmosphere_irradiance.h) on one definition of "up".
vec3 atmOrigin(vec3 worldPos) {
    return vec3(0.0, worldPos.y - uAtmSeaLevel + uAtmPlanetRadius, 0.0);
}

// Distance to where a ray leaves a sphere of radius R centred at the origin.
// Returns -1 when the ray never reaches it. `ro` is relative to the centre.
float atmExitDistance(vec3 ro, vec3 rd, float R) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    return -b + sqrt(d);
}

// Distance to where a ray first hits a sphere, or -1 if it misses or the hit
// is behind. Used to find the ground, which is what gives the planet an edge.
float atmHitDistance(vec3 ro, vec3 rd, float R) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    float t = -b - sqrt(d);
    return t >= 0.0 ? t : -1.0;
}

// Rayleigh and Mie phase functions. Rayleigh is nearly isotropic and is what
// makes the sky blue away from the sun; Mie is sharply forward-scattering and
// is what puts the white glare around it and the haze along the horizon.
float atmPhaseR(float mu) {
    return 3.0 / (16.0 * 3.14159265) * (1.0 + mu * mu);
}

float atmPhaseM(float mu, float g) {
    float g2 = g * g;
    float d  = 1.0 + g2 - 2.0 * g * mu;
    return 3.0 / (8.0 * 3.14159265) * ((1.0 - g2) * (1.0 + mu * mu))
         / ((2.0 + g2) * pow(max(d, 1e-4), 1.5));
}

// Integrated density along a ray, for both species at once: x = Rayleigh,
// y = Mie. Marched rather than solved because the closed form (a Chapman
// function) is only worth its complexity when this runs per pixel per step,
// and here it runs a handful of times.
vec2 atmOpticalDepth(vec3 ro, vec3 rd, float tMax, const int steps) {
    float dt = tMax / float(steps);
    vec2  sum = vec2(0.0);
    for (int i = 0; i < steps; ++i) {
        float h = length(ro + rd * (dt * (float(i) + 0.5))) - uAtmPlanetRadius;
        h = max(h, 0.0);
        sum += exp(-h / vec2(uAtmScaleHeightR, uAtmScaleHeightM));
    }
    return sum * dt;
}

vec3 atmExtinction(vec2 od) {
    return exp(-(uAtmBetaR * od.x + uAtmBetaM * ATM_MIE_EXTINCTION * od.y));
}

// In-scattered radiance along [0, tMax] of the ray, plus the transmittance
// over that same segment.
//
// `ro` is the view position relative to the PLANET CENTRE. Passing tMax the
// distance to a surface gives aerial perspective; passing the distance out of
// the atmosphere gives the sky. One function, so a ridge at the horizon and the
// sky just above it cannot drift apart.
vec3 atmScatter(vec3 ro, vec3 rd, float tMax, const int steps,
                const int sunSteps, out vec3 transmittance) {
    float dt = tMax / float(steps);
    float mu = dot(rd, uAtmSunDir);
    float pr = atmPhaseR(mu);
    float pm = atmPhaseM(mu, uAtmMieG);

    float atmR = uAtmPlanetRadius + uAtmThickness;

    vec2 odView = vec2(0.0);          // accumulated along the view ray
    vec3 sumR = vec3(0.0);
    float sumM = 0.0;

    for (int i = 0; i < steps; ++i) {
        vec3  p = ro + rd * (dt * (float(i) + 0.5));
        float h = max(length(p) - uAtmPlanetRadius, 0.0);
        vec2  density = exp(-h / vec2(uAtmScaleHeightR, uAtmScaleHeightM)) * dt;
        odView += density;

        // Light reaching this sample from the sun. Points in the planet's
        // shadow contribute nothing, which is what makes the terminator and
        // the reddening at low sun happen on their own rather than being
        // faked with a colour ramp.
        vec3 sunTrans = vec3(0.0);
        if (atmHitDistance(p, uAtmSunDir, uAtmPlanetRadius) < 0.0) {
            float ts = atmExitDistance(p, uAtmSunDir, atmR);
            if (ts > 0.0) {
                vec2 odSun = atmOpticalDepth(p, uAtmSunDir, ts, sunSteps);
                sunTrans = atmExtinction(odSun + odView);
            }
        }
        sumR += sunTrans * density.x;
        sumM += dot(sunTrans, vec3(0.3333)) * density.y;
    }

    transmittance = atmExtinction(odView);
    return uAtmSunColor * (uAtmBetaR * sumR * pr + uAtmBetaM * sumM * pm);
}

// Sky radiance for a view ray that leaves the atmosphere (or hits the ground).
// `worldPos` is a world-space position; the planet is centred below the origin
// so that world Y == uAtmSeaLevel sits on the surface.
vec3 atmSky(vec3 worldPos, vec3 rd, const int steps, const int sunSteps) {
    vec3 ro = atmOrigin(worldPos);
    float atmR = uAtmPlanetRadius + uAtmThickness;

    float tMax = atmExitDistance(ro, rd, atmR);
    if (tMax <= 0.0) return vec3(0.0);          // outside, looking away

    // Stop at the ground so downward rays do not integrate through the planet.
    float tGround = atmHitDistance(ro, rd, uAtmPlanetRadius);
    if (tGround > 0.0) tMax = min(tMax, tGround);

    vec3 tr;
    return atmScatter(ro, rd, tMax, steps, sunSteps, tr);
}

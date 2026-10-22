// The atmosphere's model on the GPU (docs/scene-world-architecture.md §7;
// Hillaire 2020): its media, the geometry of the shell, and the tables'
// parametrisations, word for word those of scene/world/atmosphere.h (the
// CPU half, which bro_atmosphere_test checks the tables against).
//
// The includer defines ATM, an AtmosphereGpu (common/scene_atmosphere_params.glsl):
// lighting.atm in the lit shaders (scene_lighting.glsl), the compute block's
// in the table makers (lighting/atmosphere_*.comp); and ATM_TRANSMITTANCE_TEX,
// the transmittance table as a sampler2D expression (a combined sampler, or
// a sampler2D(texture, sampler) constructor, which must sit at the lookup,
// so the functions below name the macro rather than take the table as an
// argument). An includer that reads the sky's light defines ATM_SKYLIGHT_TEX,
// the sky-light table as a sampler3D expression, the same way. Every table holds
// radiance for a sun of illuminance 1; what reads it scales by
// ATM.sunIlluminance, so a dimmer or redder sun is not a remade table.
//
// Air space: positions about the planet's centre. On a flat body the planet
// sits straight below the eye (ATM.origin = (0, eye height above sea level +
// radius, 0)) so the air's up is the world's +Y everywhere; on a round body
// (ATM.shell.w 1) the planet is the body and ATM.origin is the eye less its
// centre (scene/world/atmosphere.h).

const float ATM_PI = 3.14159265359;
#define ATM_RB (ATM.shell.x)
#define ATM_RT (ATM.shell.y)

// The tables' sizes (scene/world/atmosphere.h).
const vec2 ATM_TRANSMITTANCE_SIZE = vec2(256.0, 64.0);
const vec2 ATM_MULTISCATTER_SIZE = vec2(64.0, 32.0);
const vec2 ATM_SKYVIEW_SIZE = vec2(256.0, 128.0);
const vec2 ATM_SKYLIGHT_SIZE = vec2(64.0, 32.0);
const float ATM_SKYLIGHT_LAYERS = 5.0;
const float ATM_FROXEL_XY = 32.0;
const float ATM_FROXEL_SLICES = 32.0;
// The tables of light (multiple scattering, sky view, aerial in-scatter, the
// sky's light) hold it times this, so twilight's (1e-8 of the sun near the
// eye) stays above half float's smallest values; the brightest (the haze
// about the sun, about 1) stays far below its largest.
const float ATM_TABLE_SCALE = 1024.0;

// A camera-relative position in air space.
vec3 atmOrigin(vec3 rel) { return ATM.origin.xyz + rel; }

// --- Media -----------------------------------------------------------------------

struct AtmMedium {
    vec3 scatterR;    // Rayleigh scattering
    float scatterM;   // Mie scattering
    vec3 extinction;  // everything that takes light out of the beam
};

// The air at radius r: nothing above the shell's top.
AtmMedium atmMedium(float r) {
    AtmMedium m;
    float h = r - ATM_RB;
    if (h > ATM_RT - ATM_RB) {
        m.scatterR = vec3(0.0);
        m.scatterM = 0.0;
        m.extinction = vec3(0.0);
        return m;
    }
    h = max(h, 0.0);
    float dR = exp(-h / ATM.rayleigh.w);
    float dM = exp(-h / ATM.mie.w);
    float dO = max(0.0, 1.0 - abs(h - ATM.ozone.w) / ATM.shell.z);
    m.scatterR = ATM.rayleigh.rgb * dR;
    m.scatterM = ATM.mie.x * dM;
    m.extinction = m.scatterR + (ATM.mie.x + ATM.mie.y) * dM + ATM.ozone.rgb * dO;
    return m;
}

// Rayleigh: nearly isotropic, what makes the sky blue away from the sun.
float atmPhaseR(float nu) { return 3.0 / (16.0 * ATM_PI) * (1.0 + nu * nu); }
// Mie (Cornette-Shanks): forward, the glare about the sun and the haze.
float atmPhaseM(float nu, float g) {
    float g2 = g * g;
    float d = max(1.0 + g2 - 2.0 * g * nu, 1e-4);
    return 3.0 / (8.0 * ATM_PI) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * d * sqrt(d));
}

// --- The shell ----------------------------------------------------------------------

// Where a ray from `ro` leaves the sphere of radius R about the centre; -1 if never.
float atmSphereFar(vec3 ro, vec3 rd, float R) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    return -b + sqrt(d);
}

// Where a ray first meets the sphere (0 from inside); -1 if it never does.
float atmSphereNear(vec3 ro, vec3 rd, float R) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    if (c <= 0.0) return 0.0;
    if (b > 0.0) return -1.0;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    return -b - sqrt(d);
}

// The first hit of the planet's surface ahead, -1 if none.
float atmGroundHit(vec3 ro, vec3 rd) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - ATM_RB * ATM_RB;
    float d = b * b - c;
    if (d < 0.0) return -1.0;
    float t = -b - sqrt(d);
    return t >= 0.0 ? t : -1.0;
}

// --- The transmittance table: (mu, r), the air from a point to the top ---------

float atmUnitToTexel(float x, float size) { return 0.5 / size + x * (1.0 - 1.0 / size); }
float atmTexelToUnit(float u, float size) { return (u - 0.5 / size) / (1.0 - 1.0 / size); }

vec2 atmTransmittanceUv(float r, float mu) {
    float Rb = ATM_RB, Rt = ATM_RT;
    float H = sqrt(max(Rt * Rt - Rb * Rb, 0.0));
    float rho = sqrt(max(r * r - Rb * Rb, 0.0));
    float disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    float d = max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
    float dMin = Rt - r, dMax = rho + H;
    float xMu = dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0;
    float xR = H > 0.0 ? rho / H : 0.0;
    return vec2(atmUnitToTexel(clamp(xMu, 0.0, 1.0), ATM_TRANSMITTANCE_SIZE.x),
                atmUnitToTexel(clamp(xR, 0.0, 1.0), ATM_TRANSMITTANCE_SIZE.y));
}

void atmTransmittanceRMu(vec2 uv, out float r, out float mu) {
    float Rb = ATM_RB, Rt = ATM_RT;
    float xMu = atmTexelToUnit(uv.x, ATM_TRANSMITTANCE_SIZE.x);
    float xR = atmTexelToUnit(uv.y, ATM_TRANSMITTANCE_SIZE.y);
    float H = sqrt(max(Rt * Rt - Rb * Rb, 0.0));
    float rho = H * xR;
    r = sqrt(rho * rho + Rb * Rb);
    float dMin = Rt - r, dMax = rho + H;
    float d = dMin + xMu * (dMax - dMin);
    mu = d <= 0.0 ? 1.0 : clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// The air's transmittance from radius r (inside the shell) along mu to the top.
vec3 atmTransmittanceToTop(float r, float mu) {
    return textureLod(ATM_TRANSMITTANCE_TEX,atmTransmittanceUv(clamp(r, ATM_RB, ATM_RT), mu), 0.0).rgb;
}

// From a point anywhere (above the shell too) along a ray that does not hit
// the planet: the air the ray crosses on its way out.
vec3 atmTransmittanceRay(vec3 ro, vec3 rd) {
    float r = length(ro);
    if (r > ATM_RT) {
        float t = atmSphereNear(ro, rd, ATM_RT);
        if (t < 0.0) return vec3(1.0);
        ro += rd * t;
        r = ATM_RT;
    }
    return atmTransmittanceToTop(r, dot(ro, rd) / r);
}

// How much of the sun's disc stands above the planet's horizon from radius
// r with the sun at mu_s: 0 under it, 1 clear, smooth across the disc.
float atmPlanetShadow(float r, float muS) {
    float s = clamp(ATM_RB / max(r, ATM_RB), 0.0, 1.0);   // sin of the horizon's zenith angle
    float muH = -sqrt(max(1.0 - s * s, 0.0));
    float alpha = max(ATM.sunIlluminance.w, 1e-4);
    return smoothstep(-alpha, alpha, (muS - muH) / max(s, 1e-3));
}

// The sun's light at radius r with the sun at mu_s, for a sun of 1.
vec3 atmSunTransmittance(float r, float muS) {
    return atmTransmittanceToTop(r, muS) * atmPlanetShadow(r, muS);
}

// --- The sun's angle in the (mu_s, ...) tables: texels near the horizon ---------

float atmSunUnit(float muS) {
    float m = clamp(muS, -1.0, 1.0);
    return 0.5 + 0.5 * sign(m) * sqrt(abs(m));
}
float atmSunFromUnit(float x) {
    float s = 2.0 * x - 1.0;
    return sign(s) * s * s;
}

// --- The multiple-scattering table: (mu_s, r) ----------------------------------

vec2 atmMultiScatterUv(float r, float muS) {
    return vec2(atmUnitToTexel(atmSunUnit(muS), ATM_MULTISCATTER_SIZE.x),
                atmUnitToTexel(clamp((r - ATM_RB) / (ATM_RT - ATM_RB), 0.0, 1.0), ATM_MULTISCATTER_SIZE.y));
}

// --- The sky-view table: (azimuth from the sun, zenith angle) at the eye --------

void atmSkyViewHorizon(float r, out float zTop, out float zHorizon) {
    float rr = max(r, ATM_RB + 1.0);
    zHorizon = ATM_PI - asin(min(ATM_RB / rr, 1.0));
    zTop = rr > ATM_RT ? ATM_PI - asin(min(ATM_RT / rr, 1.0)) : 0.0;
}

vec2 atmSkyViewUv(float r, float cosZ, float cosAz) {
    float zTop, zH;
    atmSkyViewHorizon(r, zTop, zH);
    float z = acos(clamp(cosZ, -1.0, 1.0));
    float y;
    if (z < zH) {
        float c = clamp((z - zTop) / (zH - zTop), 0.0, 1.0);
        y = 0.5 * (1.0 - sqrt(1.0 - c));
    } else {
        float c = clamp((z - zH) / (ATM_PI - zH), 0.0, 1.0);
        y = 0.5 + 0.5 * sqrt(c);
    }
    float x = sqrt(clamp(0.5 - 0.5 * cosAz, 0.0, 1.0));
    return vec2(atmUnitToTexel(x, ATM_SKYVIEW_SIZE.x), atmUnitToTexel(y, ATM_SKYVIEW_SIZE.y));
}

void atmSkyViewDirection(float r, vec2 uv, out float cosZ, out float cosAz) {
    float zTop, zH;
    atmSkyViewHorizon(r, zTop, zH);
    float x = atmTexelToUnit(uv.x, ATM_SKYVIEW_SIZE.x);
    float y = atmTexelToUnit(uv.y, ATM_SKYVIEW_SIZE.y);
    float z;
    if (y < 0.5) {
        float c = 1.0 - (1.0 - 2.0 * y) * (1.0 - 2.0 * y);
        z = zTop + c * (zH - zTop);
    } else {
        float c = (2.0 * y - 1.0) * (2.0 * y - 1.0);
        z = zH + c * (ATM_PI - zH);
    }
    cosZ = cos(z);
    cosAz = 1.0 - 2.0 * x * x;
}

// The local frame of a point: up, and the sun's horizontal direction (any
// horizontal when the sun stands at the zenith or nadir).
void atmSunFrame(vec3 up, out vec3 xAxis, out vec3 yAxis, out float muS) {
    vec3 sun = ATM.sunDir.xyz;
    muS = dot(up, sun);
    vec3 h = sun - up * muS;
    float l = length(h);
    if (l > 1e-5) {
        xAxis = h / l;
    } else {
        vec3 a = abs(up.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
        xAxis = normalize(a - up * dot(a, up));
    }
    yAxis = cross(up, xAxis);
}

// --- The sky's light: SH by (mu_s, altitude) ---------------------------------------

vec2 atmSkyLightUv(float r, float muS) {
    float h = clamp((r - ATM_RB) / (ATM_RT - ATM_RB), 0.0, 1.0);
    return vec2(atmUnitToTexel(atmSunUnit(muS), ATM_SKYLIGHT_SIZE.x),
                atmUnitToTexel(sqrt(h), ATM_SKYLIGHT_SIZE.y));
}

// The six SH basis functions kept (scene/world/atmosphere.h), at local (x, y, z).
void atmShBasis6(vec3 d, out float b[6]) {
    b[0] = 0.282095;
    b[1] = 0.488603 * d.z;
    b[2] = 0.488603 * d.x;
    b[3] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    b[4] = 1.092548 * d.x * d.z;
    b[5] = 0.546274 * (d.x * d.x - d.y * d.y);
}

#ifdef ATM_SKYLIGHT_TEX
// The sky's light at camera-relative `pos` (scene/world/atmosphere.h): the
// six SH coefficients a channel in the sun's frame there, times the sun.
void atmSkyLightSh(vec3 pos, out vec3 c[6], out mat3 frame) {
    vec3 p = atmOrigin(pos);
    float r = length(p);
    vec3 up = p / r;
    vec3 xAxis, yAxis;
    float muS;
    atmSunFrame(up, xAxis, yAxis, muS);
    frame = mat3(xAxis, yAxis, up);   // columns: local x, y, z in air space
    vec2 uv = atmSkyLightUv(r, muS);
    vec4 l0 = textureLod(ATM_SKYLIGHT_TEX, vec3(uv, 0.1), 0.0);
    vec4 l1 = textureLod(ATM_SKYLIGHT_TEX, vec3(uv, 0.3), 0.0);
    vec4 l2 = textureLod(ATM_SKYLIGHT_TEX, vec3(uv, 0.5), 0.0);
    vec4 l3 = textureLod(ATM_SKYLIGHT_TEX, vec3(uv, 0.7), 0.0);
    vec4 l4 = textureLod(ATM_SKYLIGHT_TEX, vec3(uv, 0.9), 0.0);
    vec3 E = ATM.sunIlluminance.rgb / ATM_TABLE_SCALE;
    c[0] = l0.rgb * E;
    c[1] = vec3(l0.a, l1.rg) * E;
    c[2] = vec3(l1.ba, l2.r) * E;
    c[3] = l2.gba * E;
    c[4] = l3.rgb * E;
    c[5] = vec3(l3.a, l4.rg) * E;
}

// The irradiance over pi the coefficients `c` (in the sun's `frame`) give a
// surface facing `n`.
vec3 atmShIrradiance(vec3 c[6], mat3 frame, vec3 n) {
    float b[6];
    atmShBasis6(n * frame, b);   // into the sun's frame
    vec3 e = ATM_PI * c[0] * b[0] + (2.0 * ATM_PI / 3.0) * (c[1] * b[1] + c[2] * b[2]) +
             (ATM_PI / 4.0) * (c[3] * b[3] + c[4] * b[4] + c[5] * b[5]);
    return max(e, vec3(0.0)) / ATM_PI;
}

// The radiance along `dir` the coefficients hold (smooth: the ambient's
// specular on a surface no probe covers).
vec3 atmShRadiance(vec3 c[6], mat3 frame, vec3 dir) {
    float b[6];
    atmShBasis6(dir * frame, b);
    return max(c[0] * b[0] + c[1] * b[1] + c[2] * b[2] + c[3] * b[3] + c[4] * b[4] + c[5] * b[5], vec3(0.0));
}
#endif

// --- The aerial-perspective froxels --------------------------------------------------

// The volume's w for a distance d, and how much of the first slice's air a
// point nearer than it gets (scene/world/atmosphere.h, froxelW).
float atmFroxelW(float maxDist, float d, out float nearWeight) {
    float s = sqrt(max(d, 0.0) / maxDist) * ATM_FROXEL_SLICES;
    float first = maxDist / (ATM_FROXEL_SLICES * ATM_FROXEL_SLICES);
    nearWeight = clamp(d / first, 0.0, 1.0);
    return clamp((s - 0.5) / ATM_FROXEL_SLICES, 0.5 / ATM_FROXEL_SLICES, 1.0 - 0.5 / ATM_FROXEL_SLICES);
}

// Interleaved gradient noise (Jimenez 2014) in [0, 1): the lookups' dither.
float atmNoise(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

// The light of a piece of cloud (sky/clouds.frag's march and the shells,
// sky/cloud_shells.frag, so a heap handed over is lit as it was): the
// droplets' phase, and a piece's light integrated over its length.
// Included after sky/scene_cloud_layer.glsl and common/scene_lighting.glsl.

// The sky's and the ground's light a cloud scatters, of their irradiance:
// the radiance a uniform sky gives (1/pi), not the irradiance itself, which
// lit a heap's shaded side and underside nearly as its sunlit top.
const float CL_AMBIENT = 1.0 / PI;

float clHg(float nu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));
}
// The phase of water droplets, as two lobes: a strong forward one (the
// silver lining) and a weak backward one.
float clPhase(float nu, float k) { return mix(clHg(nu, 0.8 * k), clHg(nu, -0.3 * k), 0.2); }

// What a view carries.
struct ClMarch {
    vec3 L;          // light scattered toward the eye
    float T;         // what is left of the view
    float depthSum;  // distance weighted by what each piece took
    float weightSum;
};

// One piece's light, integrated over its length `dt` (Hillaire 2016): `p`
// its point lit (air space), `tau` the deck's depths from there toward the
// sun and up, `about` the cloud's optical depth about the point (the
// powder's), `ph` the octaves' phases.
// (The deep body's diffuse light falls with the depth toward the sun as
// CLOUD_DIFFUSE_FALL has it. clIntegrate's `deep` is how much of that light
// the piece takes: the sun over its horizon.)
void clIntegrate(inout ClMarch m,vec3 p, float tm, float dt, float sigma, vec2 tau, float about, vec3 s,
                 float nu, float ph[6], float albedo, vec3 E, vec3 skyAbove, vec3 groundBelow, float deep) {
    float r = length(p);
    vec3 up = p / r;
    float h = clamp((r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x), 0.0, 1.0);
    // The sun at the sample: through the air (and the planet's shadow),
    // then through the deck.
    vec3 sunAir = atmSunTransmittance(r, dot(up, s));
    // Multiple scattering's octaves (Wrenninge), the deeper ones dimmed
    // where the cloud is thin about the sample (the powder: little light
    // has scattered about at a cloud's edge, so seen with the sun behind
    // the eye its edges are darker than its body; toward the sun, the edge
    // is the bright silver lining).
    // (Six octaves, each's extinction 0.6 of the last's and its light 0.8:
    // a thick cloud seen from the sun's side sends back about what a white
    // diffuser would, as a deck from orbit does, while a heap's far side
    // and underside, deep from the sun, stay grey; at 0.4 the deep octaves
    // lit them nearly as its top.)
    float powder = mix(1.0 - exp(-about), 1.0, clamp(nu, 0.0, 1.0));
    float ms = 0.0, a = 1.0, b = 1.0;
    for (int o = 0; o < 6; ++o) {
        ms += b * ph[o] * exp(-tau.x * a) * (o == 0 ? mix(1.0, powder, 0.5) : powder);
        a *= 0.6;
        b *= 0.8;
    }
    // (The octaves past the sixth, b / (1 - 0.8) of the light, isotropic:
    // the deep body's diffuse light, which a heap's top seen from above
    // sends back (the six octaves' forward lobes about half a white
    // diffuser's light) and its sides and base too, and which fills the
    // cloud seen from within. It falls with the depth from the sun as
    // diffusion does, not as the octaves' exponentials: a thick cloud lets
    // through 1 / (1 + 0.75 (1 - g) tau) of the flux, g some 0.85, so 500 m
    // under a heap's sunlit top the inside is a bright haze, as it is, and
    // a base 2 km under it a grey a tenth of the top's. The sixth octave's
    // exp(-0.05 tau) left the inside dark grey, so it was given only to
    // views looking down from over the deck.)
    ms += deep * b / 0.2 * (0.25 / PI) / (1.0 + CLOUD_DIFFUSE_FALL * tau.x) * powder;
    // The sky's light, less what the cloud above hides (a floor for what
    // its body scatters about), and the ground's from below.
    float sees = mix(0.2, 1.0, exp(-0.15 * tau.y));
    vec3 ambient = (skyAbove * (0.35 + 0.65 * h) * sees + groundBelow * (0.6 - 0.4 * h)) * CL_AMBIENT;
    vec3 S = albedo * sigma * (E * sunAir * ms + ambient);
    float Tr = exp(-sigma * dt);
    m.L += m.T * (S - S * Tr) / sigma;
    float took = m.T * (1.0 - Tr);
    m.depthSum += took * tm;
    m.weightSum += took;
    m.T *= Tr;
}

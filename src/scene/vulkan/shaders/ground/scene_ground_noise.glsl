// Band-limited noise for the ground and what covers it: every pattern fades
// an octave finer than about two pixels to its mean, so the turf has texture
// at any height without shimmering. Included by scene_grass.glsl, so the
// world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl) have it in
// every cover shader. Needs scene_camera.glsl (sceneWindNoise).

float groundHash(ivec2 p, int k) {
    uint h = uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(k) * 0xcb1ab31fu;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// An octave of frequency `freq`'s share at footprint `fp`: none as its
// period nears two footprints.
float groundSeen(float freq, float fp) { return clamp(0.5 / (freq * fp) - 1.0, 0.0, 1.0); }

// Value noise in [0, 1], an octave of period 1 / freq faded to its mean
// (0.5) as the period nears two pixels of footprint `fp` (m).
float groundOctave(vec2 p, float freq, float fp) {
    float seen = groundSeen(freq, fp);
    if (seen <= 0.0) return 0.5;
    return mix(0.5, sceneWindNoise(p * freq) * 0.5 + 0.5, seen);
}

// Several octaves from `freq` up, each half the amplitude.
float groundFbm(vec2 p, float freq, float fp, int octaves) {
    float s = 0.0, a = 0.5, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        s += a * groundOctave(p + float(i) * 17.31, freq, fp);
        n += a;
        a *= 0.5;
        freq *= 2.03;
    }
    // Stretched to about one octave's range: a sum crowds toward its mean.
    return clamp(0.5 + (s / n - 0.5) * 2.2, 0.0, 1.0);
}

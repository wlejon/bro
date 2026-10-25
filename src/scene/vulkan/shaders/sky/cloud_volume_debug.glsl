// The volume's debug views (clouds.frag built with CL_DEBUG_VOLUME, the
// CloudsVolumeFrag variant the march draws with under
// BRO_CLOUD_VOLUME_DEBUG; vulkan/sky/scene_cloud_volume.h). The view is
// clouds.volE2.w:
//
//   1  the volume drawn instead of the forms: the deck's spans walked on
//      the volume alone (its distance where it is over a cell, else half a
//      cell at a time), each piece's extinction the finest level's whose
//      cell is as wide as the pixel or wider (filtered), lit by its depth
//      toward the sun, so the volume can be judged against the clouds;
//   2  its channels as slices: a panel a level (left to right) and a
//      channel (top to bottom: the extinction of a heart's, the distance of
//      the reach's, the sun's transmittance's complement, the occupancy),
//      each the level's window from above at clouds.volN.w metres over the
//      anchor's deck bottom, in the bins' colours (black none, red, yellow,
//      green, cyan, blue, white all);
//   3  the same, each panel the window's x and height through the eye's row.
//
// The includer is clouds.frag, after clIntegrate (sky/cloud_volume.glsl
// included).

vec3 clVolBins(float v) {
    return v <= 0.0 ? vec3(0) : v < 0.125 ? vec3(1, 0, 0) : v < 0.25 ? vec3(1, 1, 0) : v < 0.5 ? vec3(0, 1, 0)
         : v < 0.75 ? vec3(0, 1, 1) : v < 1.0 ? vec3(0, 0, 1) : vec3(1);
}

vec4 clVolSlices(int view) {
    vec2 uv = gl_FragCoord.xy / camera.viewport.xy;
    vec2 at = uv * vec2(float(CL_VOL_LEVELS), 4.0);
    int k = clamp(int(at.x), 0, CL_VOL_LEVELS - 1), ch = clamp(int(at.y), 0, 3);
    vec2 f = fract(at);
    if (min(f.x, f.y) < 0.006 || max(f.x, f.y) > 0.994) return vec4(0.0, 0.0, 0.0, 1.0);
    float cell = cloudVolCell(k);
    ivec3 c = ivec3(clouds.volLevel[k].xyz);
    ivec3 g;
    if (view == 2) {
        g = ivec3(c.x - CL_VOL_SIDE / 2 + int(f.x * float(CL_VOL_SIDE)),
                  c.y - CL_VOL_SIDE / 2 + int((1.0 - f.y) * float(CL_VOL_SIDE)), int(floor(clouds.volN.w / cell)));
        if (g.z < c.z - CL_VOL_DEPTH / 2 || g.z >= c.z + CL_VOL_DEPTH / 2) return vec4(vec3(0.02) * 1e6, 1.0);
    } else {
        g = ivec3(c.x - CL_VOL_SIDE / 2 + int(f.x * float(CL_VOL_SIDE)), int(floor(clouds.volEye.y / cell)),
                  c.z - CL_VOL_DEPTH / 2 + int((1.0 - f.y) * float(CL_VOL_DEPTH)));
    }
    vec4 t = cloudVolTexel(k, (vec3(g) + 0.5) * cell);
    float v = ch == 0 ? t.x / clouds.forms.x
            : ch == 1 ? t.y / (float(CL_VOL_REACH) * cell)
            : ch == 2 ? 1.0 - exp(-t.z)
            : t.w;
    return vec4(clVolBins(v) * 1e6, 1.0);
}

// The deck's spans walked on the volume alone (view 1), into `m`.
void clVolMarch(inout ClMarch m, vec3 ro, vec3 rd, vec2 spanA, vec2 spanB, vec3 s, vec3 E, float nu, float ph[6],
                float albedo, float pixel) {
    vec3 dl = cloudVolDir(cloudToBody(rd));
    vec3 skyAbove = vec3(0.0), groundBelow = vec3(0.0);
    bool sky = false;
    int steps = 0;
    for (int span = 0; span < 2; ++span) {
        vec2 sp = span == 0 ? spanA : spanB;
        float t = sp.x;
        while (t < sp.y && steps < 600 && m.T > CL_OPAQUE) {
            ++steps;
            vec3 v = clouds.volEye.xyz + dl * t;
            int k = cloudVolLevel(v, pixel * t);
            if (k >= CL_VOL_LEVELS) break;
            float cell = cloudVolCell(k);
            float d = cloudVolFree(k, v);
            if (d > cell) {
                t += d;
                continue;
            }
            float L = min(0.5 * cell, sp.y - t);
            float x = t + 0.5 * L;
            vec4 c = cloudVolSample(k, clouds.volEye.xyz + dl * x);
            if (c.x > 0.0) {
                if (!sky) {
                    clSkyAt(ro, rd * x, skyAbove, groundBelow);
                    sky = true;
                }
                ClAt w = clAt(ro + rd * x);
                float tauUp = clCache(w, clLod(w, cell)).z;
                clIntegrate(m, ro + rd * x, x, L, c.x, vec2(c.z, tauUp), c.x * max(120.0, cell), s, nu, ph, albedo, E,
                            skyAbove, groundBelow, 0.0);
            }
            t += L;
        }
    }
}

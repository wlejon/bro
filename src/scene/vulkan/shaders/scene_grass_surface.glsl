// A flowering culm's surface (mesh.frag's SCENE_GRASS build): the card
// scene_grass.glsl stands up for it is cut per pixel into its bare stem and
// its head of spikelets. Everything comes from the blade's varyings (where
// on the card, the head's form, its ripeness, the stem's share): nothing is
// stored per culm.

const float GRASS_SURF_CULM_HEAD = 0.76;   // scene_grass.glsl's GRASS_CULM_HEAD
// scene_grass.glsl's grassHeadColor (keep in step): fresh, green flushed
// with purple; ripe, straw.
vec3 grassSurfHeadColor(float ripe) { return mix(vec3(0.105, 0.11, 0.065), vec3(0.40, 0.33, 0.18), ripe); }

float grassSurfHash(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)) + ivec2(4096));
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// The head's half width at t (0 its foot .. 1 its tip), in head widths: a
// loose panicle a lozenge widest low down, a dense spike a slim cylinder
// rounded at both ends, `form` between them.
float grassHeadHalf(float t, float form) {
    float s = sin(3.14159265 * clamp(t, 0.0, 1.0));
    float pan = 0.5 * pow(max(sin(3.14159265 * pow(clamp(t, 0.0, 1.0), 0.75)), 0.0), 0.8);
    float spike = 0.19 * pow(max(s, 0.0), 0.3);
    return mix(pan, spike, form);
}

// uv: across (head widths, 0 the middle) and along the card (0 .. 1); info:
// scene_grass.glsl's GrassBlade.info. Returns the share of the pixel the
// culm covers, and its colour (`stem` the stem's) and how much light it lets
// through.
float grassCulmSurface(vec2 uv, vec4 info, vec3 stem, out vec3 albedo, out float translucency) {
    vec2 fw = max(fwidth(uv), vec2(1e-5));
    float X = uv.x, Y = uv.y;
    float seed = fract(info.x) * 97.0;
    // Unpacked (scene_grass.glsl's GrassBlade.info): the card is at least
    // about a pixel wide, so a far culm's head and stem cover only kHead and
    // kStem of what is cut from it.
    float stemShare = floor(info.w) / 1000.0, kStem = fract(info.w) / 0.99;
    float form = floor(info.y) / 15.0, kHead = fract(info.y) / 0.99;
    // The stem, under its head and running up into it.
    float stemHalf = 0.5 * stemShare;
    float stemCover = clamp((stemHalf - abs(X)) / fw.x + 0.5, 0.0, 1.0) * kStem;
    albedo = stem;
    translucency = 0.35;
    if (Y < GRASS_SURF_CULM_HEAD) return stemCover;

    float ripe = info.z;
    float t = (Y - GRASS_SURF_CULM_HEAD) / (1.0 - GRASS_SURF_CULM_HEAD);
    float hw = grassHeadHalf(t, form);
    // Spikelets: rows up the head, each row a branch from the rachis rising
    // out to one side (to the outline), its spikelets small ovals along its
    // outer part; a spike's branches are short, its spikelets crowded into
    // its outline.
    float rows = mix(11.0, 26.0, form);
    float r = t * rows;
    float ri = floor(r);
    float rpx = max(fw.y * rows, 1e-5);   // rows a pixel
    float spk = 0.0, tone = 0.5, anther = 0.0, branch = 0.0;
    float ax = mix(0.06, 0.11, form), ay = mix(0.38, 0.6, form);   // an oval's half axes: across (head widths), along (rows)
    float rise = mix(1.4, 0.3, form);                              // rows a branch rises from its foot to its end
    for (int dk = -2; dk <= 1; ++dk) {
        float rk = ri + float(dk);
        float hk = max(grassHeadHalf((rk + 0.5 + 0.5 * rise) / rows, form) - ax, 0.0);
        float side = grassSurfHash(vec2(rk * 5.0 + 1.0, seed + 7.0)) < 0.5 ? -1.0 : 1.0;
        // The branch: a fine line from (0, rk + 0.5) out to (side hk, rk + 0.5 + rise).
        float sx = clamp(X * side / max(hk, 1e-4), 0.0, 1.0);
        float dr = (r - (rk + 0.5 + rise * sx)) / rpx;   // pixels off it, upward
        float bx = (X * side < 0.0 || X * side > hk) ? 1e3 : 0.0;
        branch = max(branch, clamp(0.9 - abs(dr) - bx, 0.0, 1.0) * (1.0 - form));
        for (int s = 0; s < 4; ++s) {
            vec2 hp = vec2(rk * 4.0 + float(s), seed);
            float hx = grassSurfHash(hp), hy = grassSurfHash(hp + vec2(0.0, 31.0));
            float u = mix(0.3 + 0.7 * hx, hx * 2.0 - 1.0, form);
            float cx = side * u * hk;
            float cy = rk + 0.5 + rise * abs(u) + (hy - 0.5) * mix(0.35, 0.6, form);
            vec2 dq = vec2((X - cx) / ax, (r - cy) / ay);
            float e = 1.0 - length(dq);
            // Over a pixel, soft: the oval's rim in pixels across.
            float c = clamp(e * ax / fw.x + 0.5, 0.0, 1.0);
            if (c > spk) {
                spk = c;
                tone = grassSurfHash(hp + vec2(17.0, 5.0));
                anther = step(0.72, grassSurfHash(hp + vec2(3.0, 59.0))) * smoothstep(0.2, 0.7, e);
            }
        }
    }
    // The rachis up its middle, and the branches off it.
    float rachis = max(clamp((stemHalf * 0.8 - abs(X)) / fw.x + 0.5, 0.0, 1.0) * step(t, 0.95), branch * 0.8);
    // Within its outline (a spike is solid there, its spikelets the bumps).
    float inside = clamp((hw - abs(X)) / fw.x + 0.5, 0.0, 1.0);
    float detail = max(spk * mix(1.0, inside, form), rachis);
    // Once a row of spikelets is under about two pixels the pattern fades
    // to the share of its outline it covers.
    float show = 1.0 - smoothstep(0.5, 1.0, fw.y * rows);
    float mean = inside * mix(0.35, 0.92, form);
    float cover = mix(mean, max(detail, form * inside * 0.9), show);
    cover = max(cover * kHead, stemCover * step(t, 0.3));

    // Fresh, green flushed with purple at the spikelets' tips and pale
    // anthers hanging from them; ripe, straw.
    vec3 head = grassSurfHeadColor(ripe) * mix(0.8, 1.2, mix(0.5, tone, show));
    head = mix(head, head * vec3(1.3, 0.85, 1.25), (1.0 - ripe) * 0.35 * smoothstep(0.3, 1.0, t + tone * 0.4));
    head = mix(head, vec3(0.6, 0.55, 0.28), anther * (1.0 - ripe) * show * 0.7);
    albedo = mix(stem, head, step(rachis, spk) * show + (1.0 - show));
    translucency = mix(0.42, 0.25, ripe);
    return cover;
}

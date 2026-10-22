// A flowering culm's surface (mesh.frag's SCENE_GRASS build): the card
// scene_grass.glsl stands up for it is cut per pixel into its bare stem and
// its head of spikelets. Everything comes from the blade's varyings (where
// on the card, the head's form, its ripeness, the stem's share): nothing is
// stored per culm.

const float GRASS_SURF_CULM_HEAD = 0.76;   // scene_grass.glsl's GRASS_CULM_HEAD
// scene_grass.glsl's grassHeadColor (keep in step): fresh, green flushed
// with purple; ripe, straw.
vec3 grassSurfHeadColor(float ripe) { return mix(vec3(0.105, 0.11, 0.065), vec3(0.40, 0.33, 0.18), ripe); }
// scene_grass.glsl's grassHeadFarColor (keep in step): a whole head's mean
// once its spikelets are under a pixel.
vec3 grassSurfHeadFarColor(float ripe) { return mix(vec3(0.25, 0.2, 0.19), vec3(0.42, 0.35, 0.21), ripe); }

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
float grassCulmSurface(vec2 uv, vec4 info, vec3 stem, float graze, out vec3 albedo, out float translucency) {
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
    // Under a pixel a head is its spikelets' mean with their pale anthers;
    // seen across (`graze`, 0 from above .. 1 along the ground) the light
    // caught on their glumes too: a meadow in flower seen across takes a
    // pinkish-buff haze from its heads.
    vec3 headMean = mix(grassSurfHeadColor(ripe) * vec3(1.15, 1.0, 1.15), vec3(0.6, 0.55, 0.28), 0.08 * (1.0 - ripe));
    head = mix(head, mix(headMean, grassSurfHeadFarColor(ripe), graze), 1.0 - show);
    albedo = mix(stem, head, step(rachis, spk) * show + (1.0 - show));
    translucency = mix(0.42, 0.25, ripe);
    return cover;
}

// A far fan of leaves (scene_grass.glsl's fan card): four leaves from the
// card's foot, fanning out to tips spread across it, the outer ones
// shorter (each ends inside the card's pointed top), each tapering to its
// point. uv as the culm's (across in the card's width at its middle);
// info.y a leaf's width there. Each leaf is box-filtered over the pixel's
// width, so one under a pixel covers its true share and a far sward keeps
// its cover however fine its leaves. Returns the share of the pixel they
// cover; `tone` the leaf's own lightness (0..1) where they do.
float grassFanSurface(vec2 uv, vec4 info, out float tone) {
    float fw = max(fwidth(uv.x), 1e-5);
    float X = uv.x, Y = uv.y;
    float seed = fract(info.x) * 97.0;
    float hw0 = 0.5 * fract(info.y);
    float cover = 0.0, best = 0.0;
    tone = 0.5;
    for (int k = 0; k < 4; ++k) {
        float hk = grassSurfHash(vec2(float(k) * 3.0 + 11.0, seed));
        float tipX = ((float(k) + 0.5) * 0.25 - 0.5) + (hk - 0.5) * 0.1;
        float len = min(0.75 + 0.25 * grassSurfHash(vec2(float(k) * 3.0 + 12.0, seed)), 0.97 - abs(tipX));
        if (Y > len) continue;
        float s = Y / len;
        // Bowing out a little from the foot.
        float cx = mix(0.3 * tipX, tipX, s * (1.25 - 0.25 * s));
        float w = hw0 * pow(max(1.0 - pow(s, 2.2), 0.0), 0.8) * (0.8 + 0.2 * smoothstep(0.0, 0.2, s));
        float c = max(min(cx + w, X + 0.5 * fw) - max(cx - w, X - 0.5 * fw), 0.0) / fw;
        if (c > best) {
            best = c;
            tone = grassSurfHash(vec2(float(k) * 3.0 + 13.0, seed));
        }
        cover += c * (1.0 - cover);
    }
    return min(cover, 1.0);
}

// scene_grass.glsl's grassPackColor, undone.
vec3 grassUnpackColor(float p) {
    float q = floor(p + 0.5);
    vec3 c = vec3(floor(q / 16384.0), mod(floor(q / 128.0), 128.0), mod(q, 128.0)) / 127.0;
    return c * c;
}

// A far herb's bloom over a fan (info.z 1, its colour in info.w): a round
// head at the fan's top, as wide as about half the fan and never under
// about a pixel across, cut per pixel (a fan's leaves are tall and thin, so
// a bloom drawn as the whole fan was a dash). Returns the share of the
// pixel it covers.
float grassFanBloom(vec2 uv, vec4 info, out vec3 color) {
    color = grassUnpackColor(info.w);
    vec2 fw = max(fwidth(uv), vec2(1e-5));
    float r = max(0.25 / fw.x, 0.6);   // its radius in pixels
    vec2 q = vec2(uv.x / fw.x, (1.0 - uv.y) / fw.y - r);
    return clamp(r + 0.5 - length(q), 0.0, 1.0);
}

// ---- The blade as mesh.frag lights it (its SCENE_GRASS build) --------------
// These read mesh.frag's varyings (inUV, inColor, inInstColor, inTangent,
// inVegShade) and call coverSampleMask (scene_cover_mask.glsl, which
// mesh.frag includes).

// What a blade is, and what it hands the lighting.
struct GrassSurface {
    bool culm;      // a flowering culm: its card cut into its stem and head
    bool dead;      // a dead leaf
    float trans;    // a culm's translucency (grassCulmSurface); -1: the blade's own
    vec3 up;        // the way the sward's blades stand
    float subpixel; // how far it is detail too fine to show (mesh.frag's outMark)
};

// A grass blade (scene_grass.glsl's GrassBlade): inColor what it is (its
// kind, under 0.5 a draw of its own; a culm's head, ripeness and stem
// share), inUV across it (in its width; a culm's in its head's) and
// along it. A culm's card is cut into its stem and its head.
// `baseColor` the blade's colour, `msaa` MESH_MSAA; a cut card's cover
// is kept as that share of the samples (gl_SampleMask), none discards.
GrassSurface grassSurface(vec3 worldPos, inout vec3 baseColor, bool msaa) {
    GrassSurface g;
    float grassKind = floor(inColor.x + 1e-3);
    g.culm = grassKind > 0.5 && grassKind < 1.5;
    g.dead = grassKind > 1.5 && grassKind < 2.5;
    g.trans = -1.0;
    g.up = vec3(0.0, 1.0, 0.0);
    g.subpixel = 0.0;
    // The card's width in pixels (taken before any discard).
    float acrossPx = 1.0 / max(fwidth(inUV.x), 1e-5);
    if (grassKind > 2.5) {
        // A far fan of leaves, cut into them.
        float tone;
        float cover = grassFanSurface(inUV, inColor, tone);
        // A far herb's bloom over it (scene_grass_surface.glsl's
        // grassFanBloom), in place of the blade's colour.
        float fanBloom = 0.0;
        vec3 bloomColor = vec3(0.0);
        if (inColor.z > 0.5) fanBloom = grassFanBloom(inUV, inColor, bloomColor);
        vec3 plain = baseColor / max(inInstColor.rgb, vec3(1e-4));
        baseColor = mix(baseColor * mix(0.84, 1.16, tone), plain * bloomColor, fanBloom);
        cover = max(cover, fanBloom);
        int mask = coverSampleMask(cover, msaa, uint(fract(inColor.x) * 131072.0));
        if (mask == 0) discard;
        gl_SampleMask[0] = mask;
        // As far as the dither kept the pixel, where its leaves are a pixel
        // or so wide (wider, near, it is only their edges).
        float leafPx = fract(inColor.y) * acrossPx;
        g.subpixel = max(1.0 - smoothstep(1.5, 3.0, leafPx),
                         subpixelDecided(cover) * (1.0 - smoothstep(3.0, 6.0, leafPx)));
    }
    if (g.culm) {
        vec3 culmColor;
        float graze = 1.0 - smoothstep(0.05, 0.4, -worldPos.y / max(length(worldPos.xz), 1e-3));
        float cover = grassCulmSurface(inUV, inColor, baseColor, graze, culmColor, g.trans);
        int mask = coverSampleMask(cover, msaa, uint(fract(inColor.x) * 131072.0));
        if (mask == 0) discard;
        gl_SampleMask[0] = mask;
        baseColor = culmColor;
        // As far as the dither kept the pixel, where the head is a pixel or
        // so across.
        g.subpixel = max(1.0 - smoothstep(1.5, 3.0, acrossPx),
                         subpixelDecided(cover) * (1.0 - smoothstep(3.0, 8.0, acrossPx)));
    }
    return g;
}

// The blade's normal, inside mesh.frag's vegetation blend: `N` its own,
// `vegShadeN` the bulk it is blended toward (weight `bulkWeight`).
void grassLeafNormal(inout GrassSurface g, inout vec3 N, inout vec3 vegShadeN, float bulkWeight, vec3 worldPos,
                     float camDist) {
    // Near, a live leaf is folded along its midrib: a shallow V, each
    // half turned its own way, so one half takes the light the other
    // misses. Gone by the distance a blade is a pixel or two.
    if (!g.culm && !g.dead) {
        float near = 1.0 - smoothstep(3.0, 12.0, camDist);
        if (near > 0.0) N = normalize(N - sign(inUV.x) * 0.55 * near * normalize(inTangent));
    }
    // Further off the bulk is the sward's: the vertex sends the way its
    // blades stand (scene_grass.glsl's grassSwardUp); its faces' mean
    // facet is the way to the eye bent up toward it (grassCanopyNormal,
    // GRASS_CANOPY_UP 0.5), what the sky lights and it mirrors. The sun
    // on it is its blades' mean (sceneSwardLight, below).
    g.up = bulkWeight > 1e-4 ? inVegShade.xyz / bulkWeight : vec3(0.0, 1.0, 0.0);
    if (bulkWeight > 1e-4) {
        vec3 facet = normalize(normalize(-worldPos) + 0.5 * g.up);
        vegShadeN = normalize(mix(g.up, facet, smoothstep(2.0, 14.0, camDist))) * bulkWeight;
    }
}

// A live leaf's cuticle has a sheen (what a gust's wave across a meadow
// is lit by); a dead leaf and a head are matt.
// Far off a blade is lit through its sward's mean face (turned to the
// eye, scene_grass.glsl's grassCanopyNormal), which would mirror a sun
// behind the eye back at it: the sward's sheen is broad and faint.
float grassRoughness(GrassSurface g, float roughness, float camDist) {
    return g.dead || g.culm ? 0.85 : mix(0.42, max(roughness, 0.9), smoothstep(6.0, 40.0, camDist));
}

// What the lighting takes from a blade (SceneSurface); `vegFade` how far its
// translucency has faded with distance (mesh.frag's, 0..15).
void grassSurfaceLight(inout SceneSurface s, GrassSurface g, vec3 geomN, vec3 worldPos, float vegFade, float camDist) {
    // A blade is thin: the light comes through it by its own face, not by
    // the ground's normal it is lit through further off (which faces a high
    // sun and would never let it glow). A dead leaf lets less through.
    s.thinNormal = dot(geomN, -worldPos) >= 0.0 ? geomN : -geomN;
    s.translucency = min(s.translucency * (g.dead ? 0.5 : 1.35), 1.0);
    if (g.trans >= 0.0) s.translucency = g.trans * (1.0 - vegFade / 15.0);
    // Once a blade is a pixel or so wide (from about 6 m), the sun on the
    // blades a pixel holds is their mean.
    s.sward = vec4(g.up, smoothstep(2.0, 12.0, camDist));
}

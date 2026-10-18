// Herbs: the fragment half (mesh.frag's SCENE_HERB build; the vertex half is
// scene_herb.glsl). Cuts a card of a herb plant to what it shows: a leaf's
// outline (a fern's frond of pinnae lobed into pinnules, a simple leaf with
// its teeth, a strap, a trifoliate leaf, a pinnately lobed leaf, a ribbed
// one), a stem, or a bloom (rays round a disc, a cup of broad petals, a
// head of florets, a spike, an umbel, a raceme of bells), with its colours
// from the species' record (herbRec, scene_herb_mix.glsl) and the lean of
// its normal. Every edge is antialiased against the pixel's footprint, and
// the fine cuts (pinnules, pinnae, teeth, rays, florets) fade to their mean
// fill as they shrink under a pixel, so a plant far off keeps its cover and
// tone with no shimmer.

struct HerbSurface {
    float cover;          // the share of the pixel it covers
    vec3 albedo;
    vec3 normal;          // card space: x across, y along, z out of its face
    float vein;           // 0..1: paler (a midrib, a rachis, a clover's chevron)
    float gloss;          // 0 matt .. 1 waxy
    float translucency;   // < 0: a leaf's (the vegetation default)
    float ao;
};

const float HERB_PI = 3.14159265;

// The share of a pixel `fp` wide at `x` a half-plane covers whose edge is
// `dist` inside.
float herbAA(float dist, float fp) { return clamp(0.5 + dist / max(fp, 1e-7), 0.0, 1.0); }

// The share of a pixel `fp` wide centred `x` from a line's middle that the
// line, `r` half-wide, covers (thin lines keep their own share).
float herbLine(float x, float r, float fp) {
    fp = max(fp, 1e-7);
    return clamp((min(x + 0.5 * fp, r) - max(x - 0.5 * fp, -r)) / fp, 0.0, 1.0);
}

float herbHash1(float x) { return fract(sin(x * 91.3458) * 47453.5453); }
vec2 herbHash2(vec2 p) {
    return fract(sin(vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)))) * 43758.5453);
}

// --- Leaves -----------------------------------------------------------------

// A simple leaf's half-width (of its widest) `v` of the way to its tip:
// widest a little below its middle, drawn to a point (tip 0) or rounded (1).
float herbSimpleW(float v, float tip) {
    float s = max(sin(HERB_PI * pow(clamp(v, 0.0, 1.0), 0.85)), 0.0);
    return pow(s, mix(0.75, mix(1.15, 0.45, tip), smoothstep(0.45, 0.95, v)));
}

// A serrated margin's cut (0 none .. 1 to depth) `v` along: each tooth's
// long edge rising toward the tip, its short one dropping back.
float herbTeeth(float v, float teeth) {
    float m = clamp((v - 0.1) / 0.88, 0.0, 1.0);
    float ph = fract(m * teeth);
    return m <= 0.0 ? 0.0 : (ph < 0.75 ? 1.0 - ph / 0.75 : (ph - 0.75) / 0.25);
}

// A frond: `P` metres (x out from the rachis, y up the blade from its
// base), the blade `blade` long and `halfW` wide at its widest.
float herbFrond(vec2 P, bool right, float halfW, float blade, vec4 r2, vec4 r3, float rib, float fp,
                out float vein, out vec3 normal) {
    float v = P.y / max(blade, 1e-4);
    float tip = r2.x, pinnae = max(r2.w, 1.0), lobes = r3.x;
    // The outline the pinnae fill: lanceolate (a lady fern's, narrowing to
    // its base too) or deltoid (a bracken's, widest at its base).
    float lance = pow(max(sin(HERB_PI * pow(clamp(v, 0.0, 1.0), 0.7)), 0.0), 0.8);
    float delta = pow(max(1.0 - v, 0.0), 0.85) * smoothstep(0.0, 0.06, v + 0.02);
    float env = mix(lance, delta, tip);
    float rachis = herbLine(P.x, 0.5 * rib * (1.0 - 0.6 * clamp(v, 0.0, 1.0)), fp);
    vein = rachis;
    // The pinnae: pairs along the rachis, swept toward the tip, the right
    // side's a quarter step off the left's (sub-opposite).
    float dy = blade / (pinnae + 0.5);
    float Y = P.y - 0.45 * P.x + (right ? 0.25 * dy : 0.0);
    float i = floor(Y / dy);
    float yc = (i + 0.5) * dy;
    float envC = mix(pow(max(sin(HERB_PI * pow(clamp(yc / blade, 0.0, 1.0), 0.7)), 0.0), 0.8),
                     pow(max(1.0 - yc / blade, 0.0), 0.85), tip);
    float Lp = max(halfW * envC, 1e-4);
    float a = P.x / Lp;
    float b = (Y - yc) / (0.5 * dy);
    // A pinna: narrowly triangular, a gap between it and the next, its
    // pinnules (oblong, rounded, slanting toward its tip) cut `lobes` of
    // the way to its midrib.
    float pw = 0.86 * pow(max(1.0 - a, 0.0), 0.8) * smoothstep(-0.05, 0.06, a);
    float m = clamp(Lp / (0.17 * dy), 4.0, 32.0);
    float pp = fract(a * m - abs(b) * 0.6);
    float pinnuleShow = 1.0 - smoothstep(0.6, 1.4, fp * m / Lp);
    float lobe = pow(max(sin(HERB_PI * pp), 0.0), 0.3);
    pw *= 1.0 - lobes * pinnuleShow * 0.92 * (1.0 - lobe);
    float cut = (yc < blade && i >= 0.0) ? herbAA((pw - abs(b)) * 0.5 * dy, fp) : 0.0;
    vein = max(vein, cut * herbLine(b * 0.5 * dy, 0.25 * rib, fp) * (1.0 - smoothstep(0.3, 1.0, a)));
    // Each pinna tilts a little about its midrib, the two rows of a frond
    // the other way about, and each pinnule cups: the frond is not one flat
    // sheet in the light.
    float side = right ? 1.0 : -1.0;
    normal = vec3(-0.3 * side - 0.25 * b * pinnuleShow, 0.15 * (fract(i * 0.618) - 0.5), 1.0);
    // Under a pixel the pinnae are their mean: the outline filled 55 %.
    float mean = 0.55 * herbAA((halfW * env - P.x), fp) * step(0.0, v) * step(v, 1.0);
    cut = mix(cut, mean * (1.0 - 0.3 * lobes), smoothstep(0.35, 0.9, fp / dy));
    return max(cut, rachis);
}

// A trifoliate leaf centred at `c` (metres), its leaflets `l` long, turned
// `turn`: three obovate leaflets at 120 degrees, rounded (tip 1, a clover's,
// with its pale chevron) or notched (tip 0, a wood sorrel's hearts).
float herbTrifoliate(vec2 P, vec2 c, float l, float turn, float tip, float fp, out float vein) {
    vec2 d = P - c;
    float r = length(d);
    vein = 0.0;
    if (r > l * 1.05 + fp) return 0.0;
    float best = 0.0;
    for (int k = 0; k < 3; ++k) {
        float ang = turn + float(k) * 2.0943951;
        vec2 ax = vec2(sin(ang), cos(ang));
        float a = dot(d, ax) / l;
        float b = (ax.x * d.y - ax.y * d.x) / l;
        float hw = a < 0.62 ? 0.5 * pow(max(a / 0.62, 0.0), 0.75) : 0.5 * sqrt(max(1.0 - pow((a - 0.62) / 0.38, 2.0), 0.0));
        float dist = hw - abs(b);
        float nd = (1.0 - tip) * 0.32;
        if (nd > 0.0) dist = min(dist, abs(b) - (a - (1.0 - nd)) * 0.9 + 0.01);
        dist = min(dist, a + 0.02);
        float cov = herbAA(dist * l, fp);
        if (cov > best) {
            best = cov;
            // A clover's leaflets carry a pale chevron; every leaflet a
            // midrib.
            float chev = tip * (1.0 - smoothstep(0.03, 0.07, abs(a - 0.42 - 0.9 * abs(b))));
            vein = max(chev * 0.8 * step(abs(b), 0.36), herbLine(b * l, 0.06 * l, fp) * 0.5);
        }
    }
    return best;
}

HerbSurface herbLeaf(int sp, vec2 uv, vec4 size, float stalk, float seed) {
    HerbSurface o;
    vec4 r0 = herbRec(sp, 0), r1 = herbRec(sp, 1), r2 = herbRec(sp, 2), r3 = herbRec(sp, 3);
    vec4 r4 = herbRec(sp, 4), r5 = herbRec(sp, 5), r8 = herbRec(sp, 8);
    int habit = int(r0.x + 0.5);
    int shape = int(r1.y + 0.5);
    float wd = size.y, blade = max(size.z, 1e-4);
    float halfW = 0.5 * wd;
    float X = uv.x * halfW;
    float v = stalk > 0.0 ? (uv.y - stalk) / max(1.0 - stalk, 1e-3) : uv.y;
    vec2 P = vec2(X, v * blade);
    float fp = max(max(length(vec2(dFdx(P.x), dFdy(P.x))), length(vec2(dFdx(P.y), dFdy(P.y)))), 1e-6);
    float rib = r8.w;
    o.albedo = r4.rgb;
    o.normal = vec3(0.0, 0.0, 1.0);
    o.vein = 0.0;
    o.gloss = r4.w;
    o.translucency = -1.0;
    o.ao = 1.0;
    if (uv.y < stalk) {
        // The stalk (a stipe, a petiole) up the card's middle.
        o.cover = herbLine(X, max(0.5 * rib, 0.0006), fp);
        o.albedo = mix(r5.rgb, r4.rgb, 0.3);
        o.normal = vec3(clamp(X / max(0.5 * rib, 1e-4), -1.0, 1.0) * 0.7, 0.0, 0.7);
        return o;
    }
    float tip = r2.x, teeth = r2.y, depth = r2.z;
    float u = X / max(halfW, 1e-5);
    float au = abs(u);
    float vein = 0.0;
    float cover;
    if (habit == 2 && shape == 3) {
        // A carpet's card: a cluster of trifoliate leaves on their own
        // stalks, scattered over it, each its own turn.
        cover = 0.0;
        float l = 0.5 * blade / 2.6 * 0.95;
        for (int k = 0; k < 6; ++k) {
            float fk = float(k);
            float rr = k == 0 ? 0.0 : 0.62;
            float ang = fk * 2.3999632 + seed * 6.2831853;
            vec2 c = vec2(cos(ang) * rr * (halfW - l), 0.5 * blade + sin(ang) * rr * (0.5 * blade - l));
            float vk;
            float ck = herbTrifoliate(P, c, l * (0.85 + 0.3 * herbHash1(seed * 13.0 + fk)), seed * 40.0 + fk * 1.7, tip,
                                      fp, vk);
            if (ck > cover) {
                cover = ck;
                vein = vk;
            }
        }
        cover = max(cover, 0.0);
    } else if (shape == 3) {
        float l = 0.5 * blade * 0.95;
        cover = herbTrifoliate(P, vec2(0.0, 0.5 * blade), l, 0.0, tip, fp, vein);
    } else if (shape == 0) {
        cover = herbFrond(vec2(abs(X), P.y), X > 0.0, halfW, blade, r2, r3, rib, fp, vein, o.normal);
        // A frond yellows a little toward its tip and its pinnae's edges.
        o.albedo *= mix(vec3(1.0), vec3(1.1, 1.08, 0.85), 0.5 * v);
    } else {
        float w;
        float pleat = 0.0;
        if (shape == 2) {
            // A strap: parallel-sided, narrowing to its tip.
            w = sqrt(clamp((1.0 - v) / mix(0.18, 0.4, tip), 0.0, 1.0)) * (0.7 + 0.3 * smoothstep(0.0, 0.15, v));
            float lines = abs(fract(au * 2.5 + 0.5) - 0.5);
            vein = (1.0 - smoothstep(0.0, 0.12, lines)) * (1.0 - smoothstep(0.3, 1.0, fp * 5.0 / max(halfW, 1e-4))) * 0.4;
        } else if (shape == 5) {
            // Ribbed (a plantain's): an oval with veins along its length.
            w = pow(max(sin(HERB_PI * pow(clamp(v, 0.0, 1.0), 0.9)), 0.0), mix(0.9, 0.55, tip));
            // Its ribs run base to tip, meeting at both ends (two a side
            // and the midrib), raised and pale; the blade puckers up
            // between them.
            float q = au / max(w, 1e-3);
            float lines = min(abs(q - 0.4), abs(q - 0.76));
            float show = 1.0 - smoothstep(0.3, 1.0, fp * 8.0 / max(halfW, 1e-4));
            vein = (1.0 - smoothstep(0.02, 0.1, lines * (1.2 - 0.6 * abs(v - 0.5)))) * show;
            float cellQ = q < 0.4 ? q / 0.4 : (q < 0.76 ? (q - 0.4) / 0.36 : (q - 0.76) / 0.24);
            pleat = cos(HERB_PI * clamp(cellQ, 0.0, 1.0)) * 0.35 * show * sin(HERB_PI * clamp(v, 0.0, 1.0));
        } else {
            w = herbSimpleW(v, shape == 4 ? 0.5 : tip);
            float show = 1.0 - smoothstep(0.5, 1.4, fp * max(teeth, 1.0) / blade);
            if (teeth > 0.5) w *= 1.0 - depth * show * herbTeeth(v, teeth);
            if (shape == 4) {
                // Pinnately lobed: lobes pairs, cut `lobes` deep, slanting
                // toward the tip; fine ones (a yarrow's) toothed again.
                float n = max(r2.w, 1.0);
                float lshow = 1.0 - smoothstep(0.4, 1.1, fp * n / blade);
                float ph = fract(v * n - 0.3 * au);
                float lobe = sqrt(max(sin(HERB_PI * ph), 0.0));
                float cutDepth = r3.x * lshow;
                w *= mix(1.0 - cutDepth, 1.0, lobe);
                if (r3.x > 0.9) {
                    float ph2 = fract(au * 5.0 - v * n * 0.5);
                    w *= 1.0 - 0.35 * lshow * (1.0 - smoothstep(0.0, 0.5, ph2));
                }
                w *= mix(1.0, 0.75, r3.x * (1.0 - lshow));
            }
            // Pinnate veins: a midrib and laterals curving toward the tip.
            float lat = abs(fract(v * 7.0 - au * 0.9) - 0.5);
            vein = (1.0 - smoothstep(0.0, 0.06, lat)) * smoothstep(0.1, 0.25, au) * (1.0 - smoothstep(0.75, 1.0, au / max(w, 1e-3)));
            vein *= (1.0 - smoothstep(0.3, 1.0, fp * 14.0 / blade)) * 0.5;
        }
        cover = herbAA((w - au) * halfW, fp) * step(0.0, v) * step(v, 1.02);
        vein = max(vein, herbLine(X, max(0.5 * rib * (1.0 - 0.7 * v), 0.0003), fp)) * cover;
        // The halves curve away from the midrib.
        o.normal = vec3(-u * abs(u) * 0.45 + sign(u) * pleat, 0.0, 1.0);
    }
    o.cover = cover;
    o.vein = vein;
    return o;
}

// --- Stems ------------------------------------------------------------------

HerbSurface herbStemSurface(int sp, vec2 uv, vec4 size, float share) {
    HerbSurface o;
    float wd = size.y;
    float X = uv.x * 0.5 * wd;
    float fp = max(length(vec2(dFdx(X), dFdy(X))), 1e-6);
    float r = 0.5 * share * wd;
    // A hairy stem's card is wider than the stem (HERB_STEM_HAIR_ROOM a
    // side, scene_herb.glsl), the hairs standing off it in the margin.
    float hr = herbRec(sp, 10).w;
    bool resolved = share * (1.0 + 2.0 * HERB_STEM_HAIR_ROOM * hr) > 0.999;
    float stem = resolved ? herbLine(X, r, fp) : clamp(share, 0.0, 1.0) * step(abs(uv.x), 1.0);
    o.cover = stem;
    o.albedo = herbRec(sp, 5).rgb * (0.9 + 0.2 * uv.y);
    float x = clamp(uv.x / max(share, 1e-3), -1.0, 1.0);
    o.normal = vec3(x * 0.8, 0.0, sqrt(max(1.0 - 0.64 * x * x, 0.0)));
    o.vein = 0.0;
    o.gloss = 0.2;
    o.translucency = 0.15;
    o.ao = 1.0;
    if (resolved && hr > 0.0) {
        // Hairs: fine, pale, spreading out and a little up from the stem,
        // about a millimetre apart up each side, each its own length. A
        // pixel takes the share of it they cover, a silvery fringe where
        // the stem crosses the light.
        float Y = uv.y * size.x;
        float fy = max(length(vec2(dFdx(Y), dFdy(Y))), 1e-6);
        float t = abs(X) - r;                       // out from the stem's side
        float side = X < 0.0 ? 0.37 : 0.0;
        float sp1 = 0.0011;
        float yy = Y - 0.55 * t;                    // slanting up as it leaves
        float cell = floor(yy / sp1 + side);
        float jit = herbHash1(cell * 1.71 + side * 9.0);
        float yc = (cell + 0.5 + 0.6 * (jit - 0.5) - side) * sp1;
        float hl = r * HERB_STEM_HAIR_ROOM * 2.0 * (0.5 + 0.5 * herbHash1(cell * 3.3 + side * 5.0));
        float dist = abs(yy - yc) * 0.87;
        float hair = herbLine(dist, 0.00005, max(fy, fp)) * step(0.0, t) * (1.0 - smoothstep(0.75 * hl, hl, t)) *
                     (1.0 - smoothstep(0.92, 1.0, uv.y)) * hr;
        // Hairs too fine to resolve: their mean share, a haze.
        float haze = (0.00010 / sp1) * step(0.0, t) * (1.0 - smoothstep(0.4 * hl, hl, t)) * hr *
                     smoothstep(0.5 * sp1, 2.0 * sp1, max(fy, fp));
        hair = max(hair, haze);
        float h = hair * (1.0 - stem);
        o.cover = stem + h;
        float hs = h / max(o.cover, 1e-4);
        o.albedo = mix(o.albedo, vec3(0.42, 0.44, 0.36), hs);
        o.translucency = mix(o.translucency, 0.7, hs);
        o.normal = normalize(mix(o.normal, vec3(sign(X) * 0.6, 0.0, 0.8), hs));
    }
    return o;
}

// --- Blooms -----------------------------------------------------------------

HerbSurface herbBloomSurface(int sp, vec2 uv, vec4 size, float seed, vec2 down) {
    HerbSurface o;
    vec4 r6 = herbRec(sp, 6), r7 = herbRec(sp, 7), r8 = herbRec(sp, 8);
    int form = int(r6.x + 0.5);
    float petals = max(r6.w, 3.0);
    o.albedo = r7.rgb;
    o.normal = vec3(0.0, 0.0, 1.0);
    o.vein = 0.0;
    o.gloss = 0.0;
    o.translucency = 0.5;
    o.ao = 1.0;
    o.cover = 0.0;

    if (form == 4 || form == 6) {
        // A strip up the stem's top: x across (-1..1, `size.y` m), y along.
        float len = max(size.x, 1e-4), wd = size.y;
        vec2 P = vec2(uv.x * 0.5 * wd, uv.y * len);
        float fp = max(max(length(vec2(dFdx(P.x), dFdy(P.x))), length(vec2(dFdx(P.y), dFdy(P.y)))), 1e-6);
        if (form == 4) {
            // A spike: a dense cylinder of florets, rounded at its top, a
            // ring of pale anthers where it is in flower.
            // The core: a slim cylinder of tight florets (the card is 2.2
            // of its widths, room for the anthers), its top rounded.
            float v = uv.y;
            float core = 0.45 * (0.85 + 0.15 * (1.0 - v)) * sqrt(max(1.0 - pow(max(v - 0.9, 0.0) / 0.1, 2.0), 0.0));
            float x = abs(uv.x);
            float hwm = 0.5 * wd;
            float cc = herbAA((core - x) * hwm, fp);
            // The florets: little scales up the core, dark brown.
            vec2 fc = vec2(P.x / (0.3 * wd + 1e-5), P.y / (0.12 * wd + 1e-5));
            vec2 h = herbHash2(floor(fc) + seed * 17.0);
            float show = 1.0 - smoothstep(0.4, 1.0, fp / (0.12 * wd + 1e-5));
            // The band in flower (it climbs the spike as the season goes):
            // fine filaments out of the core, each tipped by a pale anther.
            float band0 = 0.2 + 0.3 * seed;
            float band = smoothstep(band0, band0 + 0.06, v) * (1.0 - smoothstep(band0 + 0.22, band0 + 0.3, v));
            float rowH = 0.09 * wd;
            float row = floor(P.y / rowH);
            vec2 ha = herbHash2(vec2(row, sign(uv.x)) + seed * 23.0);
            float reach = mix(0.7, 0.98, ha.x);
            vec2 tipA = vec2(reach, (row + 0.5) * rowH);
            float ant = herbAA(0.07 * wd - length(vec2((x - reach) * hwm, P.y - tipA.y)), fp);
            float fil = herbLine(P.y - tipA.y, 0.0002, fp) * step(core, x) * step(x, reach);
            float anthers = band * step(0.3, ha.y) * max(ant, 0.6 * fil);
            // Under a pixel the band is a pale haze round the core.
            anthers = mix(band * 0.35 * (1.0 - smoothstep(0.75, 0.95, x)), anthers, show);
            o.cover = max(cc, anthers);
            o.albedo = cc >= anthers ? r7.rgb * (0.75 + 0.5 * h.y * show) : r8.rgb;
            float xn = clamp(uv.x / max(core, 1e-3), -1.0, 1.0);
            o.normal = cc >= anthers ? vec3(xn * 0.85, 0.0, sqrt(max(1.0 - 0.72 * xn * xn, 0.0))) : vec3(0.0, 0.3, 1.0);
            o.translucency = cc >= anthers ? 0.15 : 0.5;
            return o;
        }
        // A raceme of bells hanging from the stalk to one side (+x on the
        // card): a nodding one's (a bluebell's) hang nearly straight down
        // off its arched tip, narrow, their lobes curled back; an upright
        // spike's (a foxglove's) droop out and down, fat tubes with a lip;
        // the top ones buds, smaller.
        float n = max(r6.z, 1.0);
        float bell = size.z;
        bool secund = r7.w >= 0.5;
        float ic = floor(uv.y * n);
        float best = 0.0;
        vec3 col = r7.rgb;
        vec3 nb = vec3(0.0, 0.0, 1.0);
        // (A bell hangs up to two spacings from where it is held, down the
        // card or, on a stalk arched over, up it; the higher bells hang over
        // the lower ones' tops, so they are tried first.)
        for (int k = 3; k >= -3; --k) {
            float i = ic + float(k);
            if (i < 0.0 || i >= n) continue;
            float f = i / n;
            float bud = smoothstep(0.7, 1.0, f);
            float bi = bell * mix(1.0, 0.45, f * f);
            float yi = (i + 0.5) / n * len;
            float jit = herbHash1(i * 3.1 + seed * 19.0) - 0.5;
            // Its pedicel out to the side, then the bell hangs down (the
            // world's, on the card) and out.
            // A foxglove's bells hang mostly to one side, the odd one to the
            // other, each on its own short pedicel, a little apart.
            float sd = !secund && jit > 0.32 ? -1.0 : 1.0;
            vec2 A = vec2(sd * (secund ? 0.25 : 0.35 + 0.15 * jit) * bi, yi) + down * (0.1 * bi);
            vec2 d = normalize(down + vec2(sd * (secund ? 0.2 + 0.2 * jit : 0.6 + 0.3 * jit), 0.0));
            vec2 q = P - A;
            float a = dot(q, d) / ((secund ? 1.2 : 1.05) * bi * mix(1.0, 0.6, bud));
            float b = (d.x * q.y - d.y * q.x) / bi;
            float hw;
            if (secund) {
                // Narrow, its six lobes flaring and curling back at the mouth.
                hw = 0.13 + 0.07 * a + 0.2 * smoothstep(0.78, 1.0, a);
            } else {
                // A tube swelling to its mouth, the lower lip longer.
                hw = 0.16 + 0.13 * sqrt(max(a, 0.0)) + 0.04 * smoothstep(0.85, 1.0, a);
            }
            // Rounded where it is held, like the closed end of a tube.
            hw *= mix(1.0, 0.75, bud) * sqrt(clamp(a / 0.2 + 0.15, 0.0, 1.0));
            float lip = secund ? 0.07 * (1.0 - abs(cos(b * HERB_PI * 3.0))) : -0.12 * clamp(-b * d.x * 4.0, -1.0, 1.0);
            float mouth = 1.0 - lip;
            float dist = min(hw - abs(b), min(a, mouth - a)) * bi;
            float c = herbAA(dist, fp);
            float ped = herbLine(dot(P - vec2(0.0, yi), vec2(-d.y, d.x)), 0.0006, fp) * step(0.0, P.x * sd) *
                        step(P.x * sd, abs(A.x));
            if (c > best) {
                best = c;
                // A tube is round: darker at its sides; its mouth darker
                // inside, a foxglove's throat pale and spotted.
                float xb = clamp(b / max(hw, 1e-3), -1.0, 1.0);
                float throat = smoothstep(0.86, 0.98, a) * (1.0 - smoothstep(0.3, 0.7, abs(xb)));
                vec2 hs = herbHash2(floor(vec2(a * 24.0, xb * 6.0)) + i);
                col = r7.rgb * (0.8 + 0.25 * a) * (1.0 - 0.45 * xb * xb) * (1.0 - 0.35 * throat);
                if (!secund) {
                    col = mix(col, mix(r8.rgb * 0.8, r7.rgb * 0.35, step(0.6, hs.y)), throat * 0.6);
                    // Its shoulder in the shade of the bell above, its
                    // sides rounding away: each bell reads as itself.
                    col *= mix(0.5, 1.0, smoothstep(0.0, 0.45, a)) * (1.0 - 0.25 * xb * xb);
                }
                col = mix(col, r7.rgb * vec3(0.7, 0.9, 0.6), bud * 0.5);
                nb = vec3(xb * 0.85, 0.15, sqrt(max(1.0 - 0.72 * xb * xb, 0.0)));
            }
            best = max(best, ped * 0.8);
        }
        o.cover = best;
        o.albedo = col;
        o.normal = nb;
        o.translucency = 0.35;
        return o;
    }

    // A disc (a ray flower, a cup, an umbel) or a head: q on -1..1.
    vec2 q = uv;
    float r = length(q);
    float fq = max(max(length(vec2(dFdx(q.x), dFdy(q.x))), length(vec2(dFdx(q.y), dFdy(q.y)))), 1e-5);
    float ang = atan(q.y, q.x) + seed * 6.2831853;
    if (form == 1) {
        // Rays round a domed yellow disc.
        float rc = 0.26;
        float seg = ang * petals / 6.2831853;
        float ph = fract(seg) - 0.5;
        float Lr = 0.88 + 0.12 * herbHash1(floor(seg) + seed * 31.0);
        float rw = HERB_PI * rc / petals * 1.25;
        float dist = min(rw - r * abs(ph) * 6.2831853 / petals, Lr - r);
        float rays = herbAA(dist, fq);
        float show = 1.0 - smoothstep(0.5, 1.1, fq * petals / 6.2831853 / max(rw, 1e-3) * 0.5);
        rays = mix(0.85 * herbAA(Lr - r, fq), rays, show);
        float disc = herbAA(rc - r, fq);
        o.cover = max(rays, disc);
        o.albedo = mix(r7.rgb * (0.9 + 0.1 * herbHash1(floor(seg))), r8.rgb, disc);
        o.normal = disc > 0.5 ? vec3(q / rc * 0.5, 1.0) : vec3(q * 0.15, 1.0);
        o.translucency = mix(0.55, 0.15, disc);
        return o;
    }
    if (form == 2) {
        // A cup of broad papery petals seen at the angle it is (size.w, its
        // axis on the eye; the card faces the eye, its axis up the card):
        // the rim an ellipse, the bowl below it, and looking in, the
        // petals' inner faces round the dark eye at the bottom.
        float cv = size.w, s = sqrt(max(1.0 - cv * cv, 0.0));
        const float R = 0.75, H = 0.7, yc = 0.25;
        float ry = max(R * abs(cv), 0.03);
        vec2 qr = vec2(q.x / R, (q.y - yc) / ry);
        float a2 = atan(qr.y, qr.x);
        // The rim scalloped by the petals, notched where they meet.
        float wav = 1.0 + 0.07 * (sqrt(max(sin(HERB_PI * fract(a2 * petals / 6.2831853 + seed)), 0.0)) - 0.7);
        float eR = length(qr) / wav;
        float D = max(H * s + ry, 0.03);
        float eB = q.y <= yc ? length(vec2(q.x / (R * wav), (q.y - yc) / D)) : 2.0;
        float rim = clamp(0.5 + (1.0 - eR) / max(fwidth(eR), 1e-4), 0.0, 1.0);
        float bowl = clamp(0.5 + (1.0 - eB) / max(fwidth(eB), 1e-4), 0.0, 1.0);
        o.cover = max(rim, bowl);
        float crinkle = 0.88 + 0.12 * sin(a2 * petals * 5.0 + length(q) * 26.0 + seed * 9.0);
        // Inside where the rim is seen from above, the outside elsewhere; the
        // two blend as the cup turns edge-on (cv through 0), so a cup tipping
        // past the eye's line turns smoothly rather than flipping.
        float inside = smoothstep(-0.06, 0.06, cv) * rim;
        // Its outside: round, the petals' seams running up it, paler and
        // lit through toward the rim, greener at its base.
        float xb = clamp(q.x / (R * wav), -1.0, 1.0);
        float seam = 1.0 - 0.15 * (1.0 - smoothstep(0.0, 0.08, abs(fract(asin(xb) / HERB_PI * petals * 0.5 + seed) - 0.5) - 0.42));
        float up = clamp((q.y - (yc - D)) / max(D, 1e-3), 0.0, 1.0);
        o.albedo = r7.rgb * crinkle * seam * mix(0.65, 1.0, up);
        o.albedo = mix(o.albedo, herbRec(sp, 5).rgb, 0.4 * (1.0 - smoothstep(0.0, 0.2, up)));
        o.normal = vec3(xb * 0.8, -0.25, sqrt(max(1.0 - 0.7 * xb * xb, 0.05)));
        o.translucency = 0.75;
        if (inside > 0.0) {
            // Into the cup: the eye at its bottom, the petals' faces darker
            // toward it (a poppy's black blotches at their bases).
            vec2 eyeC = vec2(0.0, yc - 0.55 * H * s);
            float eyeR = 0.2 * R;
            float de = length(vec2(q.x, (q.y - eyeC.y) / max(abs(cv), 0.15)));
            float eye = 1.0 - smoothstep(eyeR * 0.8, eyeR * 1.2, de);
            float blotch = (1.0 - smoothstep(eyeR * 1.2, eyeR * 2.0, de)) * 0.7;
            vec3 inAlbedo = mix(r7.rgb * crinkle * mix(0.7, 1.05, smoothstep(0.0, 1.0, eR)), r8.rgb, max(eye, blotch));
            o.albedo = mix(o.albedo, inAlbedo, inside);
            o.normal = normalize(mix(o.normal, vec3(-qr.x * 0.35, -0.2, 0.9), inside));
            o.translucency = mix(o.translucency, 0.4, inside);
        }
        return o;
    }
    if (form == 5) {
        // An umbel (a yarrow's corymb): a low dome of tiny florets on a
        // spray of stalks, drawn as seen (the card faces the eye, the dome's
        // axis up it at size.w, its cosine to the eye). The dome is an
        // oblate ellipsoid cut flat underneath, traced in the card's frame
        // (x across, y up the card, z toward the eye; the axis is
        // (0, s, cv)); florets lie over its top in a grid across it, the
        // shadowed cluster showing between them, only florets at its rim.
        float cv = size.w, s = sqrt(max(1.0 - cv * cv, 0.0));
        const float R = 0.95, Hd = 0.24, hcut = -0.05;
        vec2 p = q;
        float A = s * s / (R * R) + cv * cv / (Hd * Hd);
        float B = 2.0 * p.y * s * cv * (1.0 / (Hd * Hd) - 1.0 / (R * R));
        float C = p.x * p.x / (R * R) + p.y * p.y * (cv * cv / (R * R) + s * s / (Hd * Hd)) - 1.0;
        float disc = B * B - 4.0 * A * C;
        float z = (-B + sqrt(max(disc, 0.0))) / (2.0 * A);
        float hgt = p.y * s + z * cv, w = p.y * cv - z * s;
        bool top = disc > 0.0 && hgt >= hcut;
        if (disc > 0.0 && !top && abs(cv) > 0.05) {
            // Seen from below: the flat underside, where the stalks fan out.
            float zb = (hcut - p.y * s) / cv;
            w = p.y * cv - zb * s;
            hgt = hcut;
            top = false;
        }
        float rh = length(vec2(p.x, w)) / R;
        // Its outline, soft over a pixel (the ellipsoid's edge, in q).
        float sil = clamp(0.5 + 0.5 * disc / max(fwidth(disc), 1e-5), 0.0, 1.0);
        if (hgt < hcut - 1e-3) sil = 0.0;
        // The florets over its top: a grid across it (x, w), each a small
        // round head of five rays round a cream eye.
        float cells = 11.0;
        vec2 g = (vec2(p.x, w) / R + 1.0) * 0.5 * cells;
        vec2 cell = floor(g);
        vec2 hh = herbHash2(cell + seed * 11.0);
        vec2 c = cell + 0.3 + 0.4 * hh;
        vec2 dv = g - c;
        float fa = atan(dv.y, dv.x) + hh.x * 6.2831853;
        float fr = 0.4 * (0.88 + 0.12 * cos(5.0 * fa));
        // A floret's size on the card, against a pixel's (fq, in q).
        float cellQ = 2.0 * R / cells;
        float fl = herbAA((fr - length(dv)) * cellQ, fq);
        float eye = 1.0 - smoothstep(0.1, 0.2, length(dv));
        float show = 1.0 - smoothstep(0.35, 0.9, fq / cellQ);
        // Between the florets, deep in the cluster: dark; at its rim only
        // the florets stand out against what lies behind.
        float rim = smoothstep(0.72, 0.95, rh);
        float body = top ? mix(1.0, fl, rim * show) : 0.0;
        // Its stalks: from the stem's tip (under the dome's middle) out to
        // where they hold its edge, seen below and through its rim.
        float stalks = 0.0;
        vec2 tipQ = vec2(0.0, -0.3 * s);
        for (int k = 0; k < 6; ++k) {
            float a = (float(k) + herbHash1(float(k) + seed * 7.0) * 0.6) * 1.0472 + seed * 3.0;
            vec2 e = vec2(0.6 * R * cos(a), hcut * s + 0.6 * R * sin(a) * cv);
            vec2 se = e - tipQ, pp = p - tipQ;
            float t = clamp(dot(pp, se) / max(dot(se, se), 1e-6), 0.0, 1.0);
            stalks = max(stalks, herbAA(0.018 - length(pp - se * t), fq));
        }
        o.cover = max(sil * body, stalks * (1.0 - sil * body));
        vec3 shade = mix(herbRec(sp, 5).rgb * 0.8, r7.rgb * 0.35, 0.5);
        vec3 floret = mix(r7.rgb * (0.9 + 0.2 * hh.y), r8.rgb, eye * 0.8);
        o.albedo = !top ? shade : mix(mix(r7.rgb * 0.8, shade, 0.5), floret, mix(0.75, fl, show));
        if (stalks > 0.5 && sil * body < 0.5) o.albedo = herbRec(sp, 5).rgb;
        // The dome's normal (the ellipsoid's gradient in the card's frame),
        // each floret a little dome of its own.
        vec3 gN = vec3(p.x / (R * R), w / (R * R) * cv + hgt / (Hd * Hd) * s, -w / (R * R) * s + hgt / (Hd * Hd) * cv);
        vec3 bump = vec3(dv * 0.6 * fl * show, 0.0);
        o.normal = top ? normalize(normalize(gN) + bump) : vec3(0.0, -0.6, 0.8);
        o.ao = top ? mix(0.55, 1.0, smoothstep(hcut, Hd, hgt)) * mix(0.7, 1.0, mix(fl, 1.0, 1.0 - show)) : 0.45;
        o.translucency = 0.3;
        return o;
    }
    // A head (form 3): a ball of florets facing the eye, browner below.
    float edge = 0.9 + 0.08 * sin(ang * 11.0 + seed * 20.0);
    o.cover = herbAA(edge - r, fq);
    vec2 g = q * 6.0;
    vec2 h = herbHash2(floor(g) + seed * 7.0);
    float show = 1.0 - smoothstep(0.4, 1.0, fq * 6.0);
    o.albedo = r7.rgb * mix(1.0, 0.75 + 0.45 * h.x, show) * mix(0.75, 1.0, smoothstep(-0.9, 0.2, q.y));
    float z = sqrt(max(1.0 - r * r, 0.0));
    o.normal = vec3(q * 0.9, z + 0.1);
    o.translucency = 0.2;
    o.ao = mix(0.75, 1.0, z);
    return o;
}

// The card's surface: `info` (role 0 leaf / 1 stem / 2 bloom, species,
// seed), `size` (scene_herb.glsl's HerbVertex::size), `extra` (a stem's
// cover of its card, or a leaf's stalk share).
HerbSurface herbSurface(vec4 info, vec2 uv, vec4 size, float extra, vec2 down) {
    int role = int(info.x + 0.5);
    int sp = clamp(int(info.y + 0.5), 0, GRASS_HERB_MAX - 1);
    if (role == 1) return herbStemSurface(sp, uv, size, extra);
    if (role == 2) return herbBloomSurface(sp, uv, size, info.z, normalize(down + vec2(0.0, -1e-4)));
    return herbLeaf(sp, uv, size, extra, info.z);
}

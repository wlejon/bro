// Herbs: the low plants of a meadow and a forest floor (ferns, wildflowers,
// forbs, ground cover), built where they stand: the vertex half of
// GrassNode's herb rings (mesh_herb.vert; the fragment half is
// scene_herb_surface.glsl). Needs scene_camera.glsl, scene_grass.glsl (the
// field, the floor, the influence map; herbPick from scene_herb_mix.glsl)
// and scene_vegetation.glsl (a cantilever's response to the wind).
//
// No plant has data of its own. An instance is a cell of the herbs' grid;
// herbPick says which species grows there, if any, and its hashes the rest.
// The instance's mesh is several CARDS (strips of a few segments, x across
// -0.5..0.5, y along 0..1), and its plant takes them in turn:
//   blooms  one a stem, at its tip: a disc facing along the stem (a daisy's
//           rays, a poppy's cup, a yarrow's umbel), a ball facing the eye (a
//           clover's head), or a strip up the stem's top (a plantain's
//           spike, a raceme of hanging bells);
//   stems   from the root (or strewn over a carpet's patch), rising,
//           nodding at the top as the species does;
//   leaves  round the root (a rosette, a fern's fronds), up the stems in
//           pairs (a leafy herb), or strewn (a carpet), each rising at its
//           species' angle and arching over toward its tip; the first card
//           row is its stalk, the rest its blade, whose outline the
//           fragment stage cuts.
// The far herb ring draws fewer cards: the blooms and stems, and a share
// of the leaves, each larger so the plant covers as much.
//
// Each stem and leaf is a cantilever (broflora's herbSway: its frequency
// and its tip deflection at the reference wind, from its thickness and
// what it carries) answering the scene's wind field as a tree's limbs do
// (vegLevel), its drag cut by the canopy overhead; it bends from its base
// along its length. A leaf on a stem rides the stem's bend.

struct HerbVertex {
    vec3 pos;      // camera-relative
    vec3 normal;   // the card's
    vec3 side;     // across the card
    vec3 ground;   // the ground's normal under the plant
    vec3 tint;     // the plant's own tone
    vec2 uv;       // the card: x across (-1..1), y along (0 base .. 1 tip; a bloom's own)
    vec4 info;     // x role (0 leaf, 1 stem, 2 bloom), y species, z seed, w a stem's cover of its card / a leaf's stalk share
    vec4 size;     // the card's length and width (m), the leaf's own length (m), the plant's height over the ground at the vertex (share)
    float ao;
    float bulk;
    vec2 down;     // the world's down on the card (x across, y along): where a raceme's bells hang
};

const float HERB_GOLDEN = 2.3999632;
const float HERB_STEPS = 6.0;   // the steps a stalk's curve is followed in

// A stalk (a stem, a leaf's length): where it leaves, the way it leaves, the
// bend it takes toward its tip at rest (`bend`, with profile `bendFrom`:
// past that share of the length, or along it as t^2 when 0) and in the wind
// (`wind`: the tip's deflection over the length, bending as a cantilever),
// its length.
struct HerbStalk {
    vec3 base;
    vec3 d0;
    vec3 bend;
    float bendFrom;
    vec3 wind;
    float len;
};

vec3 herbTangent(HerbStalk s, float t) {
    float b = s.bendFrom > 0.0 ? smoothstep(s.bendFrom - 0.12, s.bendFrom + 0.3, t) : t * t;
    return normalize(s.d0 + s.bend * b + s.wind * (2.0 * t));
}

// The point `t` of the way along stalk `s`, and its direction there.
vec3 herbPoint(HerbStalk s, float t, out vec3 tangent) {
    vec3 p = s.base;
    float ds = 1.0 / HERB_STEPS;
    for (int k = 0; k < int(HERB_STEPS); ++k) {
        float a = float(k) * ds;
        if (a >= t) break;
        float step = min(ds, t - a);
        p += herbTangent(s, a + 0.5 * step) * (step * s.len);
    }
    tangent = herbTangent(s, t);
    return p;
}

// A unit vector at azimuth `a` (radians) and `lean` from upright.
vec3 herbDir(float a, float lean) { return vec3(cos(a) * sin(lean), cos(lean), sin(a) * sin(lean)); }

// The wind's deflection (world metres, at the tip) of a cantilever of
// frequency `freq`, sway `sway` and length `len` on phase `phase`.
// A stalk bent far leans out of the wind (it shows less of itself to it,
// across less of the flow), so its deflection saturates: past about half
// its length it gains little more.
vec3 herbSwing(float freq, float sway, float len, float zeta, float phase, VegWind w, float bob) {
    vec3 x = vegLevel(VegCantilever(freq, sway, len), zeta, phase, w, bob);
    return x / (1.0 + length(x) / max(0.6 * len, 1e-3));
}

// Stem `k` of a plant of species `sp` at camera-relative `root`.
HerbStalk herbStem(int sp, int k, vec3 root, vec4 h, float size, VegWind w, float shelter, vec3 push) {
    vec4 r0 = herbRec(sp, 0), r7 = herbRec(sp, 7), r11 = herbRec(sp, 11);
    int habit = int(r0.x + 0.5);
    float hk = fract(h.z * 7.13 + float(k) * 0.618);
    float a = float(k) * HERB_GOLDEN + h.y * 6.2831853;
    vec3 out3 = vec3(cos(a), 0.0, sin(a));
    float off = habit == 2 ? r0.z * size * sqrt(fract(hk * 3.7)) : habit == 1 ? 0.02 + 0.04 * hk : 0.012 * size;
    HerbStalk s;
    s.base = root + out3 * off;
    s.d0 = herbDir(a, 0.05 + 0.14 * hk);
    s.len = r0.y * size * (0.82 + 0.3 * hk);
    // A nodding stem (a bluebell's) droops at its top.
    s.bend = (out3 * 0.3 - vec3(0.0, 1.0, 0.0)) * (r7.w * 2.2);
    s.bendFrom = 0.62;
    vec3 x = herbSwing(r11.x, r11.y * shelter, s.len, 0.08, hk, w, 0.2);
    s.wind = x / max(s.len, 0.01) + push;
    return s;
}

bool herbVertex(int id, int vid, vec3 v, out HerbVertex o) {
    o.pos = vec3(0.0);
    o.normal = vec3(0.0, 1.0, 0.0);
    o.side = vec3(1.0, 0.0, 0.0);
    o.ground = vec3(0.0, 1.0, 0.0);
    o.tint = vec3(1.0);
    o.uv = vec2(0.0);
    o.info = vec4(0.0);
    o.size = vec4(0.0);
    o.ao = 1.0;
    o.bulk = 0.3;
    o.down = vec2(0.0, -1.0);

    vec3 rect = grassRect();
    int W = max(int(rect.z), 1);
    ivec2 c = ivec2(int(rect.x) + id % W, int(rect.y) + id / W);
    float cell = grass.herbInfo.y;
    vec4 h = vec4(grassHash(c, 101), grassHash(c, 102), grassHash(c, 103), grassHash(c, 104));
    vec2 rootXZ = (vec2(c) + h.xy) * cell;
    vec3 eye = camera.eyeWorld.xyz;
    float d = length(vec2(length(rootXZ - eye.xz), grass.lod.w));

    // The ring, handing over through a dithered band, a plant at a time.
    float fin = grass.herbRing.x <= 0.0 ? 1.0 : smoothstep(grass.herbRing.x - grass.herbRing.z, grass.herbRing.x, d);
    float fout = 1.0 - smoothstep(grass.herbRing.y - grass.herbRing.w, grass.herbRing.y, d);
    if (h.w > fin || h.w > fout) return false;
    if (!grassInField(rootXZ)) return false;

    // Which card of the instance this is, and whether the far ring keeps it.
    int K = int(grass.herbShape.x + 0.5);
    int vpc = max(int(grass.herbShape.y + 0.5), 1);
    int j = vid / vpc;

    // Off screen: a box round the cell as tall as the tallest herb.
    vec4 f = grassFieldAt(rootXZ);
    vec4 cp = camera.viewProj * vec4(rootXZ.x - eye.x, f.r + 0.5 - eye.y, rootXZ.y - eye.z, 1.0);
    float m = cp.w * 0.15 + 2.2;
    if (cp.w < -2.5 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;

    vec2 fh = grassFloorHerbsAt(rootXZ);
    int sp = herbPick(rootXZ, fh.x, fh.y, grassHash(c, 105), grassHash(c, 106));    if (sp < 0) return false;

    vec4 r0 = herbRec(sp, 0), r1 = herbRec(sp, 1), r3 = herbRec(sp, 3), r6 = herbRec(sp, 6);
    vec4 r11 = herbRec(sp, 11);
    int habit = int(r0.x + 0.5);
    int nl = int(r0.w + 0.5), ns = int(r1.x + 0.5);
    int form = int(r6.x + 0.5);
    int nb = form > 0 ? ns : 0;
    int keptLeaves = clamp(K - nb - ns, 1, max(nl, 1));
    // The far ring's few cards: a flowering plant keeps three of its leaves
    // (else a daisy is one broad leaf under its flowers), dropping stems and
    // their blooms for them, the ones it keeps the larger.
    float bloomWiden = 1.0;
    int want = min(nl, 3);
    if (keptLeaves < want && ns > 1) {
        int ks = clamp((K - want) / (form > 0 ? 2 : 1), 1, ns);
        bloomWiden = sqrt(float(ns) / float(ks));
        ns = ks;
        nb = form > 0 ? ns : 0;
        keptLeaves = clamp(K - nb - ns, 1, max(nl, 1));
    }
    if (j >= nb + ns + keptLeaves) return false;
    float widen = min(sqrt(float(max(nl, 1)) / float(keptLeaves)), 1.6);
    int role = j < nb ? 2 : j < nb + ns ? 1 : 0;
    int k = role == 2 ? j : role == 1 ? j - nb : j - nb - ns;

    // The plant: its size, its tone, the ground under it, the wind at it.
    float size = 0.8 + 0.4 * h.z;
    vec3 root = vec3(rootXZ.x - eye.x, f.r - eye.y, rootXZ.y - eye.z);
    vec3 gn = grassGroundNormal(rootXZ);
    o.ground = gn;
    float tone = 0.85 + 0.3 * fract(h.z * 13.1 + h.x);
    o.tint = vec3(tone) * mix(vec3(1.06, 1.0, 0.9), vec3(0.94, 1.0, 1.1), fract(h.y * 5.7 + h.w));
    VegWind w;
    {
        vec2 dw = camera.windField.xy;
        float u = camera.windField.z, t = camera.windParams.x;
        w.dir = vec3(dw.x, 0.0, dw.y);
        w.side = vec3(-dw.y, 0.0, dw.x);
        w.p0 = vegPressure(u + 4.0 * sceneWindGustAtTime(rootXZ, t, true));
        w.p1 = vegPressure(u + 4.0 * sceneWindGustAtTime(rootXZ, t - 0.4, false));
        w.p2 = vegPressure(u + 4.0 * sceneWindGustAtTime(rootXZ, t - 1.3, false));
        w.t = t;
    }
    // Under a closed canopy the wind near the ground is a fraction of the open's.
    float shelter = 1.0 - 0.8 * clamp(fh.x, 0.0, 1.0);
    float trample;
    vec2 lie;
    vec2 pb = grassInfluence(rootXZ, trample, lie);
    vec3 push = vec3(pb.x, 0.0, pb.y) * 1.2 + vec3(lie.x, 0.0, lie.y) * (min(trample, 1.0) * 1.5);
    vec3 toEye = normalize(-root);
    o.info = vec4(float(role), float(sp), fract(h.x * 31.7 + h.z * 7.3 + float(j) * 0.137), 0.0);

    if (role == 1) {
        // A stem: a thin strip turned to the eye, its width never under
        // about two thirds of a pixel (the fragment keeps its own share).
        HerbStalk s = herbStem(sp, k, root, h, size, w, shelter, push);
        vec3 tg;
        vec3 p = herbPoint(s, v.y, tg);
        vec3 side = cross(tg, toEye);
        side = dot(side, side) > 1e-8 ? normalize(side) : vec3(1.0, 0.0, 0.0);
        float px = 2.0 / max(abs(camera.proj[1][1]) * camera.viewport.y, 1.0);
        float dia = herbRec(sp, 5).w * (1.0 - 0.4 * v.y);
        float wd = max(dia, length(p) * px * 0.7);
        o.pos = p + side * (v.x * wd);
        o.side = side;
        o.normal = normalize(cross(side, tg));
        o.uv = vec2(v.x * 2.0, v.y);
        o.info.w = dia / wd;
        o.size = vec4(s.len, wd, s.len, v.y);
        o.ao = mix(0.55, 1.0, v.y);
        // A thin stem is lit mostly as the plant it holds up, not as the
        // narrow cylinder it is (which a high sun leaves dark).
        o.bulk = 0.45;
        return true;
    }
    if (role == 2) {
        // A bloom at its stem's tip.
        HerbStalk s = herbStem(sp, k, root, h, size, w, shelter, push);
        vec3 tg;
        vec3 tip = herbPoint(s, 1.0, tg);
        float bs = r6.y * (0.85 + 0.3 * fract(h.w * 9.1 + float(k) * 0.37)) * bloomWiden;
        float nod = herbRec(sp, 7).w;
        o.bulk = 0.0;
        o.ao = 1.0;
        if (form == 4 || form == 6) {
            // A spike or a raceme: a strip up the stem's top, turned to the
            // eye about the stem, from bloomSize below the tip to the tip.
            float t0 = max(1.0 - bs / max(s.len, 1e-3), 0.0);
            float t = mix(t0, 1.0, v.y);
            vec3 tgv;
            vec3 p = herbPoint(s, t, tgv);
            vec3 side = cross(tgv, toEye);
            side = dot(side, side) > 1e-8 ? normalize(side) : vec3(1.0, 0.0, 0.0);
            // A raceme's bells hang out from the plant and down: its +x side.
            vec3 outward = normalize(vec3(tip.x - root.x, 0.0, tip.z - root.z) + vec3(1e-4, 0.0, 0.0)) -
                           vec3(0.0, 0.5, 0.0);
            if (dot(side, outward) < 0.0) side = -side;
            float bell = clamp(bs * 1.8 / max(r6.z, 1.0), 0.012, 0.05);
            float wd = form == 4 ? bs * 0.26 : bell * 2.6;
            o.pos = p + side * (v.x * wd);
            o.side = side;
            o.normal = normalize(cross(side, tgv));
            o.uv = vec2(v.x * 2.0, v.y);
            o.size = vec4((1.0 - t0) * s.len, wd, bell, 1.0);
            vec2 dn = vec2(-side.y, -tgv.y);
            o.down = dot(dn, dn) > 1e-6 ? normalize(dn) : vec2(1.0, 0.0);
            return true;
        }
        // A disc facing along the stem (dipping as it nods), or for a head a
        // ball, its card facing the eye.
        vec3 n = normalize(tg * (1.0 - nod) - vec3(0.0, nod, 0.0) + vec3(0.0, 1e-3, 0.0));
        vec3 e1, e2;
        vec3 centre = tip + tg * (0.12 * bs);
        if (form == 3) {
            n = toEye;
            centre = tip + tg * (0.45 * bs);
        }
        e1 = normalize(cross(n, abs(n.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        e2 = cross(n, e1);
        vec2 q = vec2(v.x * 2.0, v.y * 2.0 - 1.0);
        o.pos = centre + (e1 * q.x + e2 * q.y) * (0.5 * bs * (form == 3 ? 1.15 : 1.0));
        o.side = e1;
        o.normal = n;
        o.uv = q;
        o.size = vec4(bs, bs, bs, 1.0);
        return true;
    }

    // A leaf (a frond).
    vec4 r2 = herbRec(sp, 2);
    float hk = fract(h.y * 11.3 + float(k) * 0.618034);
    float hk2 = fract(h.x * 17.9 + float(k) * 0.414214);
    float L = r1.z * size * (0.85 + 0.3 * hk) * widen;
    float stalk = r3.y;
    vec3 base;
    float a;
    float rise = r3.z + (hk2 - 0.5) * 0.3;
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 baseWind = vec3(0.0);
    if (habit == 1 && ns > 0) {
        // Up a stem: in opposite pairs, each pair a quarter turn from the
        // last, smaller toward the top.
        int sk = k % ns, i = k / ns;
        int per = (keptLeaves + ns - 1) / ns;
        float ti = 0.16 + 0.72 * (float(i) + 0.5) / float(max(per, 1));
        HerbStalk st = herbStem(sp, sk, root, h, size, w, shelter, push);
        vec3 tg;
        base = herbPoint(st, ti, tg);
        up = tg;
        a = float(sk) * HERB_GOLDEN + h.y * 6.2831853 + float(i % 2) * 3.14159265 + float(i / 2) * 1.5707963;
        L *= 1.15 - 0.5 * ti;
    } else if (habit == 2) {
        // Strewn over the patch.
        a = hk * 6.2831853;
        float rr = r0.z * size * sqrt(hk2);
        base = root + vec3(cos(a * 1.7 + 1.3), 0.0, sin(a * 1.7 + 1.3)) * rr;
    } else {
        // Round the root, each a golden turn from the last; the inner (later)
        // fronds of a fern stand more upright.
        a = float(k) * HERB_GOLDEN + h.y * 6.2831853 + (hk - 0.5) * 0.4;
        base = root + vec3(cos(a), 0.0, sin(a)) * (0.01 * size);
        rise *= mix(1.0, 0.75, float(k) / float(max(nl, 1)));
    }
    vec3 out3 = vec3(cos(a), 0.0, sin(a));
    // Trampled, it lies flatter.
    rise = mix(rise, 1.45, min(trample, 1.0) * 0.8);
    HerbStalk s;
    s.base = base;
    // Its rise from the way its stem (or the vertical) goes, outward.
    vec3 outPerp = normalize(out3 - up * dot(out3, up) + vec3(0.0, 1e-4, 0.0));
    s.d0 = normalize(up * cos(rise) + outPerp * sin(rise));
    s.bend = (out3 * 1.2 - vec3(0.0, 0.9, 0.0)) * r3.w;
    s.bendFrom = stalk > 0.3 ? 0.8 * stalk : 0.0;
    s.len = L;
    vec3 x = herbSwing(r11.z, r11.w * shelter, L, 0.06, hk, w, 0.5);
    s.wind = x / max(L, 0.01) + push * 0.6;
    // The card's rows: the first is the stalk, the rest the blade.
    float S = float(max(vpc / 2 - 1, 1));
    float row = v.y * S;
    float t = stalk > 0.03 ? (row < 1.0 ? row * stalk : stalk + (row - 1.0) / max(S - 1.0, 1.0) * (1.0 - stalk)) : v.y;
    vec3 tg;
    vec3 p = herbPoint(s, t, tg);
    // Across: level, square to the leaf; a fern's frond rolls a little
    // about its rachis, and every leaf flutters about its midrib.
    vec3 side = cross(vec3(0.0, 1.0, 0.0), tg);
    side = dot(side, side) > 1e-6 ? normalize(side) : vec3(-out3.z, 0.0, out3.x);
    vec3 nrm = normalize(cross(side, tg));
    // (In a calm nothing flutters.)
    float gust = clamp(w.p0 * 3.0, 0.0, 1.0);
    float calm = clamp(camera.windField.z / 2.5, 0.0, 1.0);
    float roll = (hk2 - 0.5) * 0.5 + sin(w.t * (4.0 + 5.0 * hk) + hk * 40.0) * (0.06 * calm + 0.3 * gust) * shelter;
    vec3 sideR = side * cos(roll) + nrm * sin(roll);
    nrm = normalize(cross(sideR, tg));
    // The card's width: the leaf's (a carpet's card holds several leaves).
    float blade = (1.0 - stalk) * L;
    float wd = r1.w * L * widen * (habit == 2 ? 2.6 : 1.0);
    if (habit == 2) blade *= 2.6;
    float halfW = 0.5 * wd;
    o.pos = p + sideR * (v.x * wd);
    if (habit == 2 && t > stalk) {
        // A carpet's blade part lies on, flat, as long as it is wide.
        o.pos = herbPoint(s, stalk, tg) + normalize(vec3(tg.x, 0.0, tg.z) + 1e-4) * ((t - stalk) / (1.0 - stalk) * blade) +
                sideR * (v.x * wd);
        nrm = vec3(0.0, 1.0, 0.0);
    }
    o.side = sideR;
    o.normal = nrm;
    o.uv = vec2(v.x * 2.0, t);
    o.info.w = stalk;
    o.size = vec4(L, wd, blade, clamp((p.y - root.y) / max(r0.y * size, 0.05), 0.0, 1.0));
    // Low in the plant it is darker (its own leaves over it), and a fern's
    // frond is lit partly as the ground under it.
    o.ao = mix(0.5, 1.0, smoothstep(0.0, 0.8, o.size.w)) * mix(0.75, 1.0, t);
    o.bulk = habit == 2 ? 0.45 : 0.3;
    return true;
}

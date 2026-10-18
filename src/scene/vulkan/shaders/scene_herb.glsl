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
    // The stand it grows in: x its leaf area over a square metre of ground
    // (the leaf area index of a stand of its species at this density), y
    // the share of the stand's leaves above the vertex. The fragment stage
    // darkens it by the leaves between it and the sun and the sky, so a
    // stand of ferns is dark within and lit at its top.
    vec2 stand;
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
    // A stem's own bow at rest, its tilt growing along it (`bow` t +
    // `bow2` t^2): no stem stands ruler-straight; the two ways apart, a
    // tall one curves one way low and back near its top.
    vec3 bow;
    vec3 bow2;
    vec3 wind;
    float len;
};

// The stalk's way at `t` as a tilt from upright (xz: the way it leans,
// its length the angle): the rest pose's, then the wind's added to it, so
// the two compose as angles. (As one vector sum, normalised, a nodding
// head pushed back against its droop passed through nothing and turned
// over in a frame: the poppies popped as a walker passed.) The wind's
// tilt saturates near level, the whole short of folding back on itself.
vec3 herbTangent(HerbStalk s, float t) {
    float b = s.bendFrom > 0.0 ? smoothstep(s.bendFrom - 0.12, s.bendFrom + 0.3, t) : t * t;
    vec3 r = s.d0 + s.bend * b + (s.bow + s.bow2 * t) * t;
    float rh = length(r.xz);
    vec2 T = rh > 1e-6 ? r.xz * (atan(rh, r.y) / rh) : vec2(0.0);
    vec2 W = s.wind.xz * (2.0 * t);
    T += W * inversesqrt(1.0 + dot(W, W) * (1.0 / 1.96));   // ~atan for a small push, to 1.4 rad
    float a = length(T);
    if (a < 1e-6) return vec3(0.0, 1.0, 0.0);
    float c = a < 2.2 ? a : 2.2 + 0.6 * tanh((a - 2.2) / 0.6);
    vec2 d = T / a * sin(c);
    return vec3(d.x, cos(c), d.y);
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
    // (A leafy herb's shoots rise a hand's breadth apart, a clump.)
    float off = habit == 2 ? r0.z * size * sqrt(fract(hk * 3.7)) : habit == 1 ? 0.03 + 0.08 * hk : 0.012 * size;
    HerbStalk s;
    s.base = root + out3 * off;
    // Each stem its own lean (more on a plant of several, spreading out) and
    // its own bow: a slow curve one way along it, bending back a little or
    // on over its top, by a hash of its own.
    float hb = fract(h.w * 5.31 + float(k) * 0.381);
    float hb2 = fract(h.x * 9.77 + float(k) * 0.713);
    s.d0 = herbDir(a + (hb2 - 0.5) * 1.2, 0.03 + 0.16 * hk);
    s.len = r0.y * size * (0.82 + 0.3 * hk);
    // A nodding stem (a bluebell's) droops at its top.
    s.bend = (out3 * 0.3 - vec3(0.0, 1.0, 0.0)) * (r7.w * 2.2);
    s.bendFrom = 0.62;
    float ab = a + 1.2 + 3.6 * hb;
    vec3 bw = vec3(cos(ab), 0.0, sin(ab));
    s.bow = bw * (0.04 + 0.2 * hb2);
    s.bow2 = bw * ((hb - 0.55) * 0.35) + vec3(-bw.z, 0.0, bw.x) * ((hb2 - 0.5) * 0.15);
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
    o.stand = vec2(0.0);

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
    // The far ring keeps fewer plants with distance (as many as the whole
    // plants' range over the distance), each widened to keep their cover:
    // past the range a plant is a few pixels, and a stand reads by its mass.
    float farWiden = 1.0;
    if (grass.herbShape.z > 1.5) {
        float keep = clamp(grass.herbShape.w / max(d, 1e-3), 0.2, 1.0);
        float hk = grassHash(c, 107);
        if (hk >= keep) return false;
        // (Seen from well above, the turf takes them: herbFarSteep. Its
        // ground height from the eye's lift: a field read here, before most
        // cells are let go, cost more than the plants.)
        float horiz = max(length(rootXZ - eye.xz), 1e-3);
        if (hk >= keep * herbFarSteep(d, grass.lod.w / horiz)) return false;
        farWiden = inversesqrt(keep);
    }
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
    // A leafy herb's leaves stand in opposite pairs, a card a pair (its two
    // leaves out either way from the stem): as many again up the stems.
    if (habit == 1 && ns > 0) nl = (nl + 1) / 2;
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
    // A rosette's stem leaves (a foxglove's) take the cards left after its
    // rosette, so the far ring, short of cards, drops them first.
    vec4 r13 = herbRec(sp, 13);
    int perStem = habit == 0 && ns > 0 ? int(r13.x + 0.5) : 0;
    int nsl = perStem * ns;
    int keptAll = clamp(K - nb - ns, 1, max(nl + nsl, 1));
    if (keptLeaves >= nl) keptLeaves = keptAll;
    if (j >= nb + ns + keptLeaves) return false;
    float widen = min(sqrt(float(max(nl, 1)) / float(min(keptLeaves, max(nl, 1)))), 1.6);
    int role = j < nb ? 2 : j < nb + ns ? 1 : 0;
    int k = role == 2 ? j : role == 1 ? j - nb : j - nb - ns;

    // The plant: its size, its tone, the ground under it, the wind at it.
    float size = 0.8 + 0.4 * h.z;
    vec3 root = vec3(rootXZ.x - eye.x, f.r - eye.y, rootXZ.y - eye.z);
    vec3 gn = grassGroundNormal(rootXZ);
    o.ground = gn;
    float tone = 0.85 + 0.3 * fract(h.z * 13.1 + h.x);
    o.tint = vec3(tone) * mix(vec3(1.06, 1.0, 0.9), vec3(0.94, 1.0, 1.1), fract(h.y * 5.7 + h.w));
    // A herb's stalks are quick (a few hertz): the drag now drives them,
    // rung by its change over the last 0.4 s.
    VegWind w = vegWindLagged(rootXZ, 0.0, 0.4);
    // Under a closed canopy the wind near the ground is a fraction of the open's.
    float shelter = 1.0 - 0.8 * clamp(fh.x, 0.0, 1.0);
    float trample;
    vec2 lie;
    vec2 pb = grassInfluence(rootXZ, trample, lie);
    // A walker's push leans a herb aside, a trampled one over; a stalk is
    // pushed by a leg brushing it, not laid flat (at twice this a poppy's
    // head crossed a hand's breadth a frame as the walker came by, and
    // popped).
    vec3 push = vec3(pb.x, 0.0, pb.y) * 0.7 + vec3(lie.x, 0.0, lie.y) * (min(trample, 1.0) * 0.9);
    vec3 toEye = normalize(-root);
    o.info = vec4(float(role), float(sp), fract(h.x * 31.7 + h.z * 7.3 + float(j) * 0.137), 0.0);
    // The stand: its leaves' face over the ground (a plant's leaves seen
    // from above, times its plants a square metre at this density) and the
    // height they reach.
    vec4 r12 = herbRec(sp, 12);
    float spc = max(r12.x, cell);
    float lai = min(herbRec(sp, 10).z * 3.14159265 * r0.z * r0.z * size * size * fh.y / (spc * spc), 4.0);
    float leafTop = max(r12.w * size, 0.03);

    if (role == 1) {
        // A stem: a thin strip turned to the eye, its width never under
        // about two thirds of a pixel (the fragment keeps its own share).
        HerbStalk s = herbStem(sp, k, root, h, size, w, shelter, push);
        vec3 tg;
        vec3 p = herbPoint(s, v.y, tg);
        // Turned to the eye where it is, not where its root is: a tall stem
        // passing near the eye, its root a step off, turned by its root's
        // way, showed the eye its edge.
        vec3 side = cross(tg, normalize(-p));
        side = dot(side, side) > 1e-8 ? normalize(side) : vec3(1.0, 0.0, 0.0);
        float px = 2.0 / max(abs(camera.proj[1][1]) * camera.viewport.y, 1.0);
        float dia = herbRec(sp, 5).w * (1.0 - 0.4 * v.y);
        float wd = max(dia * (1.0 + 2.0 * HERB_STEM_HAIR_ROOM * herbRec(sp, 10).w), length(p) * px * 0.7);
        o.pos = p + side * (v.x * wd);
        o.side = side;
        o.normal = normalize(cross(side, tg));
        o.uv = vec2(v.x * 2.0, v.y);
        o.info.w = dia / wd;
        o.size = vec4(s.len, wd, s.len, v.y);
        o.stand = vec2(lai, clamp(1.0 - (p.y - root.y) / leafTop, 0.0, 1.0));
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
        float bs = r6.y * (0.85 + 0.3 * fract(h.w * 9.1 + float(k) * 0.37)) * bloomWiden * farWiden;
        float nod = herbRec(sp, 7).w;
        o.bulk = 0.0;
        o.ao = 1.0;
        o.stand = vec2(lai, clamp(1.0 - (tip.y - root.y) / leafTop, 0.0, 1.0));
        if (form == 4 || form == 6) {
            // A spike or a raceme: a strip up the stem's top, turned to the
            // eye about the stem, from bloomSize below the tip to the tip.
            // A raceme's bells: as many as its length holds, each plant its
            // own count (a young spike short, a strong one long).
            float nBells = form == 6 ? max(floor(r6.z * (0.7 + 0.55 * fract(h.w * 3.71 + float(k) * 0.29)) + 0.5), 3.0)
                                     : r6.z;
            if (form == 6) bs *= nBells / max(r6.z, 1.0);
            float t0 = max(1.0 - bs / max(s.len, 1e-3), 0.0);
            float t = mix(t0, 1.0, v.y);
            vec3 tgv;
            vec3 p = herbPoint(s, t, tgv);
            // Turned to the eye where it is (a raceme passing near the eye,
            // turned by its root's way, showed its edge: its bells drawn
            // across a card seen edge-on stretched into long pink slivers
            // floating off the spike).
            vec3 side = cross(tgv, normalize(-p));
            side = dot(side, side) > 1e-8 ? normalize(side) : vec3(1.0, 0.0, 0.0);
            // A raceme's bells hang out from the plant and down: its +x side,
            // which way round settled once for the whole card at its middle
            // (each vertex for itself, the card twisted through itself where
            // the bells hang toward or away from the eye).
            vec3 outward = normalize(vec3(s.d0.x, 0.0, s.d0.z) + vec3(1e-4, 0.0, 0.0)) -
                           vec3(0.0, 0.5, 0.0);
            vec3 tgm;
            vec3 pm = herbPoint(s, mix(t0, 1.0, 0.5), tgm);
            if (dot(cross(tgm, normalize(-pm)), outward) < 0.0) side = -side;
            float bell = clamp(bs * 1.8 / max(nBells, 1.0), 0.012, 0.05);
            // A spike is slim (a plantain's a finger's width at most, its
            // anthers standing out of it); a raceme's card holds its bells
            // hanging out to their side, so it stands mostly to that side.
            // (A foxglove's, not nodding, has the odd bell to the other side.)
            float wd = form == 4 ? max(bs * 0.075, 0.004) * 2.2 : nod >= 0.5 ? bell * 3.4 : bell * 4.2;
            float shift = form == 4 ? 0.0 : nod >= 0.5 ? 0.3 : 0.15;
            o.pos = p + side * ((v.x + shift) * wd);
            o.side = side;
            o.normal = normalize(cross(side, tgv));
            o.uv = vec2((v.x + shift) * 2.0, v.y);
            // (w: a raceme's bells, the plant's own count.)
            o.size = vec4((1.0 - t0) * s.len, wd, bell, form == 6 ? nBells : 1.0);
            // (Leaning to down the stalk as the card lies flat, where the
            // world's down on it fades: a switch there, interpolated across
            // the card, swung the bells about.)
            o.down = normalize(vec2(-side.y, -tgv.y) + vec2(0.0, -0.05));
            return true;
        }
        // A disc facing along the stem (dipping as it nods), or for a head a
        // ball, its card facing the eye.
        // A flower's face turns out from its plant as well as up (a daisy's
        // heads look about, an umbel stays near level).
        // (Out is the stem's way at rest, not its tip's from the root: a
        // stem pushed over its root swung that half a turn in a frame or
        // two, and the flower with it.)
        vec3 o3 = normalize(vec3(s.d0.x, 0.0, s.d0.z) + vec3(1e-4, 0.0, 0.0));
        float tilt = (form == 1 ? 0.75 : form == 2 ? 0.45 : form == 5 ? 0.12 : 0.0) * (0.6 + 0.8 * fract(h.w * 5.3 + float(k) * 0.71));
        vec3 n = normalize((tg + o3 * tilt) * (1.0 - nod) - vec3(0.0, nod, 0.0) + vec3(0.0, 1e-3, 0.0));
        vec3 e1, e2;
        // The eye from the flower, not from the root (a flower near the eye
        // turned to it from its root's way showed it an edge).
        toEye = normalize(-tip);
        vec3 centre = tip + tg * (0.12 * bs);
        vec2 q = vec2(v.x * 2.0, v.y * 2.0 - 1.0);
        float scale = 0.5 * bs;
        o.size = vec4(bs, bs, bs, 1.0);
        if (form == 2 || form == 5) {
            // A cup is drawn as seen: its card faces the eye, up it the
            // cup's axis, and the fragment draws the bowl and the rim's
            // ellipse at the angle it is seen (size.w, the axis on the eye).
            // So is an umbel's dome of florets, its middle over the tip.
            float cv = dot(n, toEye);
            // Up the card: the cup's axis on the eye, steadied by the
            // flower's own outward way as the axis nears the eye's line (seen
            // from straight above or below, its projection would spin).
            vec3 up2 = n - toEye * cv + (o3 - toEye * dot(o3, toEye)) * 0.05;
            e2 = dot(up2, up2) > 1e-8 ? normalize(up2) : normalize(cross(toEye, vec3(1.0, 0.0, 0.0)));
            e1 = normalize(cross(e2, toEye));
            centre = form == 2 ? tip + n * (0.7 * scale) - e2 * (0.25 * scale) : tip + n * (0.3 * scale);
            n = toEye;
            o.size.w = cv;
        } else {
            if (form == 3) {
                n = toEye;
                centre = tip + tg * (0.45 * bs);
                scale *= 1.15;
            }
            e1 = normalize(cross(n, abs(n.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
            e2 = cross(n, e1);
        }
        o.pos = centre + (e1 * q.x + e2 * q.y) * scale;
        o.side = e1;
        o.normal = n;
        o.uv = q;
        return true;
    }

    // A leaf (a frond).
    vec4 r2 = herbRec(sp, 2);
    float hk = fract(h.y * 11.3 + float(k) * 0.618034);
    float hk2 = fract(h.x * 17.9 + float(k) * 0.414214);
    // (A far plant's leaves widened more than lengthened: their area grows
    // as the plants thin, their height little.)
    float L = r1.z * size * (0.85 + 0.3 * hk) * widen * pow(farWiden, 0.7);
    float stalk = r3.y;
    vec3 base;
    float a;
    float rise = r3.z + (hk2 - 0.5) * 0.3;
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 baseWind = vec3(0.0);
    // A pair's card: its first half (v.y to 0.5) one leaf from its tip in to
    // the stem, its second the other leaf out to its tip.
    bool pair = habit == 1 && ns > 0;
    bool second = pair && v.y > 0.5;
    float vy = pair ? abs(2.0 * v.y - 1.0) : v.y;
    if (habit == 1 && ns > 0) {
        // Up a stem: in opposite pairs, each pair a quarter turn from the
        // last, closer and smaller toward the top, the last ones young
        // leaves round its tip (not a bare pole over the highest pair).
        int sk = k % ns, i = k / ns;
        int per = (keptLeaves + ns - 1) / ns;
        float ti = 0.12 + 0.84 * pow((float(i) + 0.5) / float(max(per, 1)), 0.8);
        HerbStalk st = herbStem(sp, sk, root, h, size, w, shelter, push);
        vec3 tg;
        base = herbPoint(st, ti, tg);
        up = tg;
        a = float(sk) * HERB_GOLDEN + h.y * 6.2831853 + float(i) * 1.5707963 + (hk2 - 0.5) * 0.3 +
            (second ? 3.14159265 : 0.0);
        L *= 1.15 - 0.65 * ti * ti;
    } else if (habit == 0 && k >= nl) {
        // A rosette's stem leaf (a foxglove's): alternate up the flowering
        // stem's lower half, stalkless, rising off it and arching out, the
        // lowest stemLeafSize of a rosette leaf, shrinking to a bract below
        // the flowers.
        int q = k - nl;
        int sk = q % ns, i = q / ns;
        float fi = (float(i) + 0.5) / float(max(perStem, 1));
        float ti = 0.08 + 0.42 * fi + 0.06 * (hk2 - 0.5);
        HerbStalk st = herbStem(sp, sk, root, h, size, w, shelter, push);
        vec3 tg;
        base = herbPoint(st, ti, tg);
        up = tg;
        a = float(i) * HERB_GOLDEN + h.y * 6.2831853 + float(sk) * 1.3 + (hk - 0.5) * 0.5;
        widen = 1.0;
        L = r1.z * size * (0.85 + 0.3 * hk) * r13.y * mix(1.0, 0.3, fi) * pow(farWiden, 0.7);
        stalk = 0.0;
        rise = 0.4 + 0.3 * hk2 - 0.15 * fi;
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
    s.bow = vec3(0.0);
    s.bow2 = vec3(0.0);
    s.len = L;
    vec3 x = herbSwing(r11.z, r11.w * shelter, L, 0.06, hk, w, 0.5);
    s.wind = x / max(L, 0.01) + push * 0.6;
    // The card's rows: the first is the stalk, the rest the blade.
    float S = float(max(vpc / 2 - 1, 1));
    float row = v.y * S;
    // (A one-quad card, a far plant's, runs root to tip, its stalk cut per
    // pixel; so does a pair's, its rows not falling on its stalks' ends.)
    float t = stalk > 0.03 && vpc > 4 && !pair ? (row < 1.0 ? row * stalk : stalk + (row - 1.0) / max(S - 1.0, 1.0) * (1.0 - stalk))
                                               : vy;
    vec3 tg;
    vec3 p = herbPoint(s, t, tg);
    // Across: level, square to the leaf; a fern's frond rolls a little
    // about its rachis, and every leaf flutters about its midrib.
    // (Square to the leaf's own outward way, not to the vertical: a frond
    // pushed back through upright turned over as up x tangent passed
    // through nothing, its card flipping face in a frame.)
    vec3 side0 = vec3(out3.z, 0.0, -out3.x);   // up x out3, the way it was
    vec3 side = side0 - tg * dot(side0, tg);
    side = dot(side, side) > 1e-6 ? normalize(side) : side0;
    vec3 nrm = normalize(cross(side, tg));
    // (In a calm nothing flutters.)
    float gust = smoothstep(0.0, 1.0, w.trunk * 3.0);
    float calm = clamp(camera.windField.z / 2.5, 0.0, 1.0);
    float roll = (hk2 - 0.5) * 0.5 +
                 sin(6.2831853 * sceneWindCycles((4.0 + 5.0 * hk) / 6.2831853) + hk * 40.0) * (0.06 * calm + 0.3 * gust) * shelter;
    vec3 sideR = side * cos(roll) + nrm * sin(roll);
    nrm = normalize(cross(sideR, tg));
    // The card's width: the leaf's (a carpet's card holds several leaves).
    float blade = (1.0 - stalk) * L;
    float wd = r1.w * L * widen * pow(farWiden, 0.6) * (habit == 2 ? 2.6 : 1.0);
    if (habit == 2) blade *= 2.6;
    float halfW = 0.5 * wd;
    // (A pair's second leaf turns half round, its across the first's the
    // other way: taken the first's way, the card runs on across the stem
    // untwisted, the second leaf drawn mirrored.)
    float sx = second ? -v.x : v.x;
    o.pos = p + sideR * (sx * wd);
    if (habit == 2 && t > stalk) {
        // A carpet's blade part lies on, flat, as long as it is wide.
        o.pos = herbPoint(s, stalk, tg) + normalize(vec3(tg.x, 0.0, tg.z) + 1e-4) * ((t - stalk) / (1.0 - stalk) * blade) +
                sideR * (v.x * wd);
        nrm = vec3(0.0, 1.0, 0.0);
    }
    o.side = second ? -sideR : sideR;
    o.normal = nrm;
    o.uv = vec2(v.x * 2.0, t);   // (across o.side: the second leaf mirrored)
    o.info.w = stalk;
    o.size = vec4(L, wd, blade, clamp((p.y - root.y) / max(r0.y * size, 0.05), 0.0, 1.0));
    o.stand = vec2(lai, clamp(1.0 - (p.y - root.y) / leafTop, 0.0, 1.0));
    // Low in the plant it is darker (its own leaves over it), and a fern's
    // frond is lit partly as the ground under it.
    o.ao = mix(0.5, 1.0, smoothstep(0.0, 0.8, o.size.w)) * mix(0.75, 1.0, t);
    o.bulk = habit == 2 ? 0.45 : 0.3;
    return true;
}

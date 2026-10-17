// A forest bloom's surface (mesh.frag's SCENE_FOREST_BLOOM build, the
// fragment half of mesh_forest_bloom.vert): the form shaped on its card per
// pixel from the bloom's own few numbers, nothing baked or sampled, then lit
// by mesh.frag's lighting as any surface is.
//   FLOWER  five broad petals round a small yellow centre, darker at their
//           base, cupped (the normal leans in toward the middle), the backs
//           paler;
//   HEAD    a ball of four-petalled florets (a hydrangea's mophead): traced
//           as two shells, the florets on the outer one scattered over the
//           ball by a 3D lattice (no faces, no seams), each its own turn,
//           size and tone; through the gaps between them the inner shell's
//           darker florets; the outline the outer florets' own, so it is
//           bumpy; shaded as a ball;
//   SPIKE   a bare stalk, then whorls of small florets tapering to a rounded
//           tip, the lower whorls apart (the stalk showing between), shaded
//           as a cylinder.
// Detail too fine for the pixel gives way to its mean (fwidth), and a bloom
// drawn as a dot (mesh_forest_bloom.vert's widening g > 1) is its form's
// mean over the dot, its cover the share of the dot the bloom fills.

struct ForestBloomSurface {
    float cover;          // the share of the pixel it covers (0: none)
    vec3 albedo;
    vec3 normal;          // world, unit
    vec3 at;              // camera-relative point shaded
    bool traced;          // `at` is off the card (a head's ball): its depth is written
    float translucency;
    float ao;
};

vec4 forestBloomHash(ivec3 p) {
    uvec3 q = uvec3(p) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = (q.x ^ q.y ^ q.z) * 1597334677u;
    uvec4 h = uvec4(n, n * 16807u, n * 48271u, n * 69621u);
    h = (h ^ (h >> 15u)) * 0x2c1b3c6du;
    h ^= h >> 12u;
    return vec4(h >> 8u) * (1.0 / 16777216.0);
}

// The share of a pixel inside where `d` (positive inside) changes by fwidth.
float forestBloomEdge(float d) { return clamp(d / max(fwidth(d), 1e-6) + 0.5, 0.0, 1.0); }

ForestBloomSurface forestBloomFlower(vec3 worldPos, vec2 q, vec3 n, vec3 right, vec3 up, float g, float seed,
                                     vec3 colour) {
    ForestBloomSurface s;
    s.at = worldPos;
    s.traced = false;
    s.translucency = 0.55;
    s.ao = 1.0;
    float rho = length(q);
    const vec3 yellow = vec3(0.85, 0.6, 0.06);
    if (g > 1.001) {
        // A dot: the flower's mean, three quarters of its disc.
        s.cover = rho < g ? 0.75 / (g * g) : 0.0;
        s.albedo = colour * 0.8 + yellow * 0.02;
        s.normal = n;
        return s;
    }
    float th = atan(q.y, q.x) + seed * 6.2831853;
    // Five lobes (|cos 2.5 th| has five crests round the circle), broad and
    // overlapping toward the middle, their rims a little ruffled.
    float lobe = pow(abs(cos(2.5 * th)), 0.6);
    float edge = (0.6 + 0.4 * lobe) * (1.0 + 0.025 * sin(th * 17.0 + seed * 40.0));
    s.cover = forestBloomEdge(edge - rho);
    float t = rho / max(edge, 1e-3);
    // The petals: the colour, a dark eye at their base, faint veins running
    // out, lighter toward the rim; the centre a yellow disc of anthers.
    float veins = pow(abs(sin(th * 22.5)), 10.0) * smoothstep(0.2, 0.6, t);
    vec3 petal = colour * mix(0.35, 1.0, smoothstep(0.1, 0.42, rho)) * (1.0 - 0.15 * veins) *
                 (1.0 + 0.18 * smoothstep(0.6, 1.0, t));
    float grain = forestBloomHash(ivec3(floor(q * 40.0), 7)).x;
    vec3 centre = yellow * (0.75 + 0.5 * grain);
    float c = smoothstep(0.13, 0.1, rho);
    s.albedo = mix(petal, centre, c);
    // Cupped: the petals rise toward their rims, so the face leans in toward
    // the middle; the back is the cup's outside, paler.
    vec3 nf = normalize(n - (right * q.x + up * q.y) * (0.6 * (1.0 - c)));
    bool front = dot(n, -worldPos) >= 0.0;
    s.normal = front ? nf : -nf;
    if (!front) s.albedo = mix(s.albedo, s.albedo * vec3(1.2, 1.15, 1.15) + 0.02, 0.45);
    return s;
}

// The ray from the camera (at the origin) along `d` and the sphere at `c` of
// radius `R`: the near hit's distance in `t`, and its rim measure returned
// (1 through the middle, 0 at the outline, negative past it).
float forestBloomSphere(vec3 d, vec3 c, float R, out float t) {
    float b = dot(-c, d), k = dot(c, c) - R * R;
    float disc = b * b - k;
    t = -b - sqrt(max(disc, 0.0));
    return disc / (R * R);
}

// A head's florets: a 3D lattice of cells about the ball, FOREST_BLOOM_FLORETS
// to its radius, each cell the shell passes through holding one floret,
// jittered in the cell and set on the sphere, its own size and turn. There
// are no faces or seams: a direction `sn` takes, of the florets in its cell
// and the 26 round it, the one it lies deepest inside (the topmost where
// florets overlap).
const float FOREST_BLOOM_FLORETS = 4.5;

struct ForestBloomFloret {
    bool inside;    // the direction lies on a floret
    float margin;   // how far inside its floret's outline (cell units; negative: in a gap)
    float along;    // out from the floret's eye over its size
    vec4 h;         // the floret's hash: tone, hue, tip, turn
    vec3 cup;       // its petals' cup: the offset from its eye, tangent to the ball
};

ForestBloomFloret forestBloomFloret(vec3 sn, int salt) {
    const float K = FOREST_BLOOM_FLORETS;
    vec3 u = sn * K;
    vec3 ci = floor(u);
    ForestBloomFloret f;
    f.inside = false;
    f.margin = -1e9;
    f.along = 1.0;
    f.h = vec4(0.5);
    f.cup = vec3(0.0);
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                vec3 cell = ci + vec3(x, y, z);
                vec4 h = forestBloomHash(ivec3(cell) + ivec3(salt * 7919, salt * 104729, salt));
                vec3 jp = cell + 0.5 + (h.xyz - 0.5) * 0.8;
                // Only the cells the shell runs through hold a floret.
                if (abs(length(jp) - K) > 0.6) continue;
                vec3 cn = normalize(jp);
                vec3 p3 = u - cn * K;
                p3 -= cn * dot(p3, cn);
                vec3 tu = normalize(cross(cn, abs(cn.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
                vec3 tv = cross(cn, tu);
                float rf = 0.55 + 0.15 * h.z;
                float a = h.w * 1.5707963;   // a quarter turn spans every pose of four petals
                vec2 p = vec2(dot(p3, tu), dot(p3, tv));
                p = vec2(cos(a) * p.x + sin(a) * p.y, -sin(a) * p.x + cos(a) * p.y);
                // Four round petals about a small eye, overlapping at their
                // bases: the union of four discs and the eye's.
                float pr = 0.42 * rf, po = 0.45 * rf;
                float m = 0.14 * rf - length(p);
                m = max(m, pr - length(p - vec2(po, 0.0)));
                m = max(m, pr - length(p + vec2(po, 0.0)));
                m = max(m, pr - length(p - vec2(0.0, po)));
                m = max(m, pr - length(p + vec2(0.0, po)));
                if (m > f.margin) {
                    f.margin = m;
                    f.along = length(p) / (0.87 * rf);
                    f.h = h;
                    f.cup = p3 * (0.6 / rf);
                }
            }
    f.inside = f.margin > 0.0;
    return f;
}

ForestBloomSurface forestBloomHead(vec3 worldPos, vec2 q, vec3 n, vec4 ball, float g, float seed, vec3 colour) {
    ForestBloomSurface s;
    s.at = worldPos;
    s.traced = false;
    s.translucency = 0.25;
    s.ao = 1.0;
    float mean = 0.8;
    if (g > 1.001) {
        // A dot: the ball's mean (its florets and the darker gaps), lit as a
        // ball's middle.
        s.cover = length(q) < 1.08 * g ? 0.85 / (g * g) : 0.0;
        s.albedo = colour * mean;
        s.normal = normalize(n + vec3(0.0, 0.4, 0.0));
        return s;
    }
    // Two shells along the pixel's ray: the florets' (their petals stand
    // proud of the ball) and, through the gaps between them, the ball of
    // florets below. Every pixel goes on to the end (a miss is decided
    // last), so the derivatives see the whole quad.
    vec3 d = normalize(worldPos);
    float tOut, tIn;
    float rimOut = forestBloomSphere(d, ball.xyz, ball.w * 1.05, tOut);
    float rimIn = forestBloomSphere(d, ball.xyz, ball.w * 0.95, tIn);
    vec3 snOut = normalize(d * tOut - ball.xyz), snIn = normalize(d * tIn - ball.xyz);
    int salt = int(seed * 4096.0) * 4;
    ForestBloomFloret top = forestBloomFloret(snOut, salt);
    ForestBloomFloret below = forestBloomFloret(snIn, salt + 1);
    // Fine florets give way to their mean (a pixel over several florets).
    // Measured on the card, not the ball, which foreshortens toward its rim:
    // there the florets still stand out, and are the outline.
    float keep = 1.0 - smoothstep(0.25, 0.7, length(fwidth(worldPos)) * FOREST_BLOOM_FLORETS / ball.w);
    float margin = top.margin;
    float marginAA = fwidth(margin);

    bool onTop = rimOut > 0.0 && tOut > 0.0 && (top.inside || keep < 0.5);
    bool onBelow = rimIn > 0.0 && tIn > 0.0;
    ForestBloomFloret f = onTop ? top : below;
    vec3 sn = onTop ? snOut : snIn;
    s.at = d * (onTop ? tOut : tIn);
    s.traced = true;
    // A floret: its own tone and hue, its petals lighter toward their tips
    // round a small darker eye; one below in the shade of those above.
    vec3 detail = f.inside ? colour * (0.85 + 0.3 * f.h.x) * mix(vec3(1.0), vec3(1.07, 0.94, 1.04), f.h.y) *
                                 mix(0.8, 1.05, smoothstep(0.15, 0.8, f.along)) *
                                 mix(0.5, 1.0, smoothstep(0.08, 0.16, f.along))
                           : colour * 0.4;
    if (!onTop) detail *= 0.6;
    s.albedo = mix(colour * mean, detail, keep);
    // Shaded as a ball, each floret's petals cupped about its eye and the
    // floret tipped its own way.
    s.normal = normalize(sn + ((f.h.xyz - 0.5) * 0.35 + f.cup) * (keep * (f.inside ? 1.0 : 0.0)));
    // Darker toward its underside, where the plant's leaves crowd it.
    s.ao = 0.6 + 0.4 * clamp(sn.y * 0.5 + 0.6, 0.0, 1.0);
    // The outline is the florets' own: past the ball below, only a floret
    // proud of it, its edge's share of the pixel.
    // (Seen edge on there, the florets' gaps close up into a thin band apart
    // from the ball; they thin out toward the very rim instead.)
    s.cover = onTop ? (onBelow ? 1.0 : clamp(margin / max(marginAA, 1e-6) + 0.5, 0.0, 1.0) *
                                          smoothstep(0.0, 0.2, rimOut) *
                                          mix(1.0, forestBloomEdge(rimOut), 1.0 - keep))
                    : (onBelow ? forestBloomEdge(rimIn) : 0.0);
    return s;
}

ForestBloomSurface forestBloomSpike(vec3 worldPos, vec2 q, vec3 n, vec3 right, vec3 axis, float g, float seed,
                                    float stalk, vec3 colour) {
    ForestBloomSurface s;
    s.at = worldPos;
    s.traced = false;
    s.translucency = 0.15;
    s.ao = 1.0;
    float x = q.x, y = q.y;
    const vec3 stem = vec3(0.07, 0.09, 0.05);
    const float STALK = 0.18;   // the stalk's half-width over the spike's
    float hw;                   // this height's half-width (in the spike's)
    vec3 albedo;
    vec4 h = vec4(0.5);
    // Along the spike, ten whorls (taken before any branch: a derivative).
    float t = (y - stalk) / max(1.0 - stalk, 1e-3);
    float ph = t * 10.0, wf = fract(ph);
    float keep = 1.0 - smoothstep(0.3, 0.8, fwidth(ph));
    if (y < stalk) {
        hw = STALK;
        albedo = stem;
    } else {
        // Whorls of florets tapering to a rounded tip, the lower ones apart.
        hw = mix(1.0, 0.45, t) * (0.72 + 0.28 * sin(3.14159265 * wf)) * sqrt(clamp((1.0 - t) / 0.08, 0.0, 1.0));
        bool gap = t < 0.35 && wf > 0.72;
        if (gap) hw = STALK;
        h = forestBloomHash(ivec3(int(floor(ph)), int(floor((x / max(hw, 1e-3) + 1.0) * 1.5)), int(seed * 4096.0)));
        // Each floret its own tone, a calyx greyer than its petals.
        vec3 floret = colour * (0.75 + 0.5 * h.x);
        floret = mix(floret, vec3(0.16, 0.14, 0.18) * (0.8 + 0.4 * h.z), 0.3 * h.y);
        albedo = gap ? stem : mix(colour * 0.85, floret, keep);
    }
    if (g > 1.001) {
        // A dot across: the spike's width here over the dot's.
        s.cover = abs(x) < g ? 0.85 * hw / g : 0.0;
        s.albedo = albedo;
        s.normal = n;
        return s;
    }
    s.cover = forestBloomEdge(hw - abs(x));
    s.albedo = albedo;
    // A cylinder about its axis, each floret a little its own way.
    float nx = clamp(x / max(hw, 1e-3), -1.0, 1.0);
    s.normal = normalize(right * nx + n * sqrt(max(1.0 - nx * nx, 0.0)) + axis * 0.15 +
                         (h.xyz - 0.5) * (y < stalk ? 0.0 : 0.4));
    return s;
}

ForestBloomSurface forestBloomSurface(vec3 worldPos, vec2 q, vec3 n, vec3 right, vec3 up, vec4 b0, vec4 b1,
                                      vec3 colour) {
    int form = int(b1.x + 0.5);
    if (form == 2) return forestBloomHead(worldPos, q, n, b0, b1.y, b1.z, colour);
    if (form == 3) return forestBloomSpike(worldPos, q, n, right, up, b1.y, b1.z, b1.w, colour);
    return forestBloomFlower(worldPos, q, n, right, up, b1.y, b1.z, colour);
}

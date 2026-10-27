// The heaps (docs/clouds-api.js, THE HEAPS): the deck's cumulus as things,
// one about each cell of a lattice the engine lays on the cover's cube
// (equal-angle, clouds.wind.w metres a cell at the deck's bottom; none when
// 0), carried by the wind. The world says the heap a cell holds
// (cloudHeap: its lobes, half-ellipsoids standing on their bases), the
// engine draws it: baked into the cache about the eye as the march's base
// and drawn as itself where the cache's cells are too wide to hold it, so a
// heap seen from 50 km is the heap flown through, never one the engine
// made up. Included after the world's module: a world with heaps defines
// CLOUD_HEAPS and cloudHeap (sky/scene_cloud_layer.glsl has the types).

float cloudHeapSide() {
#ifdef CLOUD_HEAPS
    return clouds.wind.w;
#else
    return 0.0;
#endif
}
// Cells along a face's edge.
int cloudHeapCells() { return max(int(floor(clouds.shell.x * 1.5707963 / max(clouds.wind.w, 1.0) + 0.5)), 1); }

// (A face and its equal-angle coordinates, -1 .. 1, for a direction.)
vec3 cloudHeapFace(vec3 up) {
    vec3 a = abs(up);
    if (a.x >= a.y && a.x >= a.z)
        return up.x >= 0.0 ? vec3(atan(vec2(-up.z, up.y) / a.x) / CLOUD_QUARTER_PI, 0.0)
                           : vec3(atan(vec2(up.z, up.y) / a.x) / CLOUD_QUARTER_PI, 1.0);
    if (a.y >= a.z)
        return up.y >= 0.0 ? vec3(atan(vec2(up.x, up.z) / a.y) / CLOUD_QUARTER_PI, 2.0)
                           : vec3(atan(vec2(up.x, -up.z) / a.y) / CLOUD_QUARTER_PI, 3.0);
    return up.z >= 0.0 ? vec3(atan(vec2(up.x, up.y) / a.z) / CLOUD_QUARTER_PI, 4.0)
                       : vec3(atan(vec2(-up.x, up.y) / a.z) / CLOUD_QUARTER_PI, 5.0);
}

// The cell a direction lies in.
ivec3 cloudHeapCellOf(vec3 up) {
    vec3 fe = cloudHeapFace(up);
    int n = cloudHeapCells();
    ivec2 ij = clamp(ivec2(floor((fe.xy * 0.5 + 0.5) * float(n))), ivec2(0), ivec2(n - 1));
    return ivec3(int(fe.z), ij);
}

// A cell's middle; and a cell's own on a face's edge (i, j past 0 .. n-1:
// the direction of its middle on the face's equal-angle chart carried on,
// which lies in a neighbouring face, and the cell there that holds it).
vec3 cloudHeapMiddle(ivec3 id) {
    int n = cloudHeapCells();
    return cloudFaceDir(id.x, (vec2(id.yz) + 0.5) / float(n) * 2.0 - 1.0);
}
ivec3 cloudHeapOwn(ivec3 id) {
    int n = cloudHeapCells();
    if (id.y >= 0 && id.z >= 0 && id.y < n && id.z < n) return id;
    return cloudHeapCellOf(cloudHeapMiddle(id));
}

// A cell's least width (m) at the deck's base: an equal-angle cell is the
// nominal side at a face's middle and 0.71 of it at a face edge's middle,
// |d dir / d e| along each of the chart's axes there.
float cloudHeapLeast(ivec2 ij) {
    int n = cloudHeapCells();
    vec2 a = tan(((vec2(ij) + 0.5) / float(n) * 2.0 - 1.0) * CLOUD_QUARTER_PI);
    vec2 k = (1.0 + a * a) * sqrt(1.0 + a.yx * a.yx) / (1.0 + dot(a, a));
    return clouds.shell.x * CLOUD_QUARTER_PI * 2.0 / float(n) * min(k.x, k.y);
}

uvec3 cloudHeapPcg(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

CloudHeapCell cloudHeapCell(ivec3 id) {
    CloudHeapCell c;
    c.id = id;
    c.up = cloudHeapMiddle(id);
    c.east = normalize(cross(abs(c.up.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), c.up));
    c.north = cross(c.up, c.east);
    c.hash = vec3(cloudHeapPcg(uvec3(id.yz, uint(id.x) * 7919u + 17u)) >> 8u) / 16777216.0;
    c.cover = cloudCoverAt(c.up);
    c.side = cloudHeapLeast(id.yz);
    return c;
}

// How far (m) a point is inside a lobe at most (negative: outside, by
// about how far; it changes about as fast as the distance, so a tall lobe
// ends over its top as sharply as at its walls): `v` from the cell's middle
// on its tangent plane, `z` metres up the deck. (The ellipsoid's distance
// as its radius, of 1, over that radius's gradient.)
float cloudLobeIn(vec4 lobe, float middle, float base, vec2 v, float z) {
    if (lobe.w <= 0.0) return -1e5;
    vec2 a = max(lobe.zw, vec2(1.0));
    vec2 e = vec2(length(v - lobe.xy), z - base - middle);
    float k0 = length(e / a), k1 = length(e / (a * a));
    float inEll = k1 > 1e-9 ? k0 * (1.0 - k0) / k1 : min(a.x, a.y);
    return min(inEll, (z - base) * 3.0);
}
float cloudHeapIn(CloudHeap h, vec2 v, float z) {
    float d = -1e5;
    for (int l = 0; l < CL_HEAP_LOBES; ++l) d = max(d, cloudLobeIn(h.lobe[l], h.middle[l], h.base[l], v, z));
    return d;
}

// (The cell about direction `up`: its face, where on it, and how far its
// nearest wall is, in metres by the cell's least width, and that width: a
// cell's neighbours are within a few percent of it.)
void cloudHeapAbout(vec3 up, out vec3 fe, out ivec2 c0, out float wall, out float side) {
    fe = cloudHeapFace(up);
    vec2 g = (fe.xy * 0.5 + 0.5) * float(cloudHeapCells());
    c0 = ivec2(floor(g));
    vec2 fr = g - vec2(c0);
    side = cloudHeapLeast(c0);
    wall = max(min(min(fr.x, 1.0 - fr.x), min(fr.y, 1.0 - fr.y)), 0.02) * side;
}

// The heaps' column over drifted body direction `up` (the heaps' map's
// texel, sky/clouds_layout.h), in metres as cloudLobeIn measures how far a
// point is inside a lobe, taken straight across and straight down: x its
// bottom (m up the deck: the lowest of the lobes over it, its base or
// where its ellipsoid's underside is over that), y how high its top stands
// over that (the highest lobe's), z how far `up` is inside the lobes'
// outline across (m; negative outside, by at least that: a lobe's outline
// its base where its middle is under it, else its widest), w how far a
// point is inside under the top a metre under it (the top's slope's
// cosine: the highest lobes'). Where no lobe is over it, x the height of
// the nearest's outline, y 0 and w its slope's cosine there, so the field
// (scene_cloud_layer.glsl's cloudHeapColumnIn) goes on smoothly across the
// outline. The 5 x 5 cells about it are searched, so a heap past them is
// more than a cell off.
vec4 cloudHeapColumn(vec3 up) {
#ifndef CLOUD_HEAPS
    return vec4(0.0, 0.0, -1e5, 1.0);
#else
    if (cloudHeapSide() <= 0.0) return vec4(0.0, 0.0, -1e5, 1.0);
    vec3 fe;
    ivec2 c0;
    float wall, side;
    cloudHeapAbout(up, fe, c0, wall, side);
    float rb = clouds.shell.x;
    float dIn = -(side + wall), zNear = 0.0, cNear = 1.0;
    // (The rate under the top: the lobes' over the texel, each weighed by
    // how near its top is to the highest, by CL_HEAP_RATE_NEAR metres, and
    // how far within its outline the texel is, so it changes smoothly from
    // texel to texel where one lobe stands over another.)
    const float CL_HEAP_RATE_NEAR = 100.0;
    // (The bottom likewise: the least of the lobes' over the texel within
    // CL_HEAP_BOTTOM_NEAR metres, each weighed by how far within its
    // outline the texel is to the fourth, so a lower lobe's bottom comes in
    // over its rim's last hundred metres or so, not at its outline.)
    const float CL_HEAP_BOTTOM_NEAR = 200.0;
    float bm = 1e9, botW = 0.0, botSum = 0.0;
    float top = -1e9, rateW = 0.0, rateSum = 0.0;
    for (int k = 0; k < 25; ++k) {
        ivec3 id = cloudHeapOwn(ivec3(int(fe.z), c0 + ivec2(k % 5, k / 5) - 2));
        CloudHeapCell c = cloudHeapCell(id);
        CloudHeap h;
        if (!cloudHeap(c, h)) continue;
        vec3 v3 = (up - c.up) * rb;
        vec2 v = vec2(dot(v3, c.east), dot(v3, c.north));
        for (int l = 0; l < CL_HEAP_LOBES; ++l) {
            vec4 lb = h.lobe[l];
            if (lb.w <= 0.0) continue;
            float r = max(lb.z, 1.0), hh = max(lb.w, 1.0), mid = h.middle[l], base = h.base[l];
            float rho = length(v - lb.xy);
            float m = mid / hh;
            float rf = mid < 0.0 ? r * sqrt(max(1.0 - m * m, 0.0)) : r;
            float d = rf - rho;
            if (d > dIn) {
                dIn = d;
                // (Its outline's height, and its slope's cosine there: a
                // vertical wall where its middle is over its base.)
                zNear = base + max(mid, 0.0);
                float sm = max(-m, 0.0);
                cNear = sm > 0.0 ? (sm / hh) * inversesqrt((1.0 - sm * sm) / (r * r) + sm * sm / (hh * hh)) : 0.0;
            }
            float q = rho / r;
            if (q >= 1.0) continue;
            float s = sqrt(1.0 - q * q);
            float t = mid + hh * s;   // (its top over its base)
            if (t <= 0.0) continue;
            float bot = base + max(mid - hh * s, 0.0);
            if (bot < bm) {
                botSum *= exp((bot - bm) / CL_HEAP_BOTTOM_NEAR);
                bm = bot;
            }
            float s2 = s * s, wb = s2 * s2;
            botW += wb;
            botSum += wb * exp((bm - bot) / CL_HEAP_BOTTOM_NEAR);
            // (Its top's slope's cosine: the ellipsoid's normal there.)
            float rate = max((s / hh) * inversesqrt(q * q / (r * r) + s * s / (hh * hh)), 0.02);
            if (base + t > top) {
                float f = exp((top - base - t) / CL_HEAP_RATE_NEAR);
                rateW *= f;
                rateSum *= f;
                top = base + t;
            }
            float wl = s * exp((base + t - top) / CL_HEAP_RATE_NEAR);
            rateW += wl;
            rateSum += wl * rate;
        }
    }
    if (top < -1e8) return vec4(zNear, 0.0, dIn, cNear);
    float zb = min(bm - CL_HEAP_BOTTOM_NEAR * log(max(botSum, 1e-30) / max(botW, 1e-30)), top);
    return vec4(zb, top - zb, dIn, rateSum / max(rateW, 1e-6));
#endif
}

// The share of the air the heaps fill at `q`, on the mean over its
// footprint (the world's cloudHeapsShare, read where the footprint is as
// wide as they are); none without heaps.
float cloudHeapsShareAt(CloudPoint q) {
#ifdef CLOUD_HEAPS
    return cloudHeapSide() > 0.0 ? cloudHeapsShare(q) : 0.0;
#else
    return 0.0;
#endif
}

// How far (m) the drifted body point `pd` (`z` metres up the deck) is
// inside the nearest heap at most (negative: outside, at least as far as
// it is said; the cells about it searched, and past them a cell's width).
float cloudHeapsIn(vec3 pd, float z) {
#ifndef CLOUD_HEAPS
    return -1e5;
#else
    if (cloudHeapSide() <= 0.0) return -1e5;
    vec3 up = normalize(pd);
    vec3 fe;
    ivec2 c0;
    float wall, side;
    cloudHeapAbout(up, fe, c0, wall, side);
    // (Lobes reach a cell from their middle, so a heap of a cell past the
    // 3 x 3 about the point's is at least its nearest wall's distance away.)
    float best = -wall;
    float rb = clouds.shell.x;
    for (int k = 0; k < 9; ++k) {
        ivec3 id = cloudHeapOwn(ivec3(int(fe.z), c0 + ivec2(k % 3, k / 3) - 1));
        CloudHeapCell c = cloudHeapCell(id);
        CloudHeap h;
        if (!cloudHeap(c, h)) continue;
        vec3 v3 = (up - c.up) * rb;
        best = max(best, cloudHeapIn(h, vec2(dot(v3, c.east), dot(v3, c.north)), z));
    }
    return best;
#endif
}

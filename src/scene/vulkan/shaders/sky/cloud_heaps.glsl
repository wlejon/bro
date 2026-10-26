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
    c.side = cloudHeapSide();
    return c;
}

// How far (m) a point is inside a lobe at most (negative: outside, by
// about how far; it changes no faster than the distance): `v` from the
// cell's middle on its tangent plane, `z` metres up the deck.
float cloudLobeIn(vec4 lobe, float middle, float base, vec2 v, float z) {
    if (lobe.w <= 0.0) return -1e5;
    vec2 e = vec2(length(v - lobe.xy), z - base - middle);
    float inEll = (1.0 - length(e / max(lobe.zw, vec2(1.0)))) * max(min(lobe.z, lobe.w), 1.0);
    return min(inEll, (z - base) * 3.0);
}
float cloudHeapIn(CloudHeap h, vec2 v, float z) {
    float d = -1e5;
    for (int l = 0; l < CL_HEAP_LOBES; ++l) d = max(d, cloudLobeIn(h.lobe[l], h.middle[l], h.base[l], v, z));
    return d;
}

// (The cell about direction `up`: its face, where on it, and how far its
// nearest wall is, of a cell.)
void cloudHeapAbout(vec3 up, out vec3 fe, out ivec2 c0, out float wall) {
    fe = cloudHeapFace(up);
    vec2 g = (fe.xy * 0.5 + 0.5) * float(cloudHeapCells());
    c0 = ivec2(floor(g));
    vec2 fr = g - vec2(c0);
    wall = max(min(min(fr.x, 1.0 - fr.x), min(fr.y, 1.0 - fr.y)), 0.02);
}

// The heaps' column over drifted body direction `up` (the heaps' map's
// texel, sky/clouds_layout.h), as cloudLobeIn measures how far a point is
// inside a lobe (its ellipsoid's radius, of 1, by its shorter half-axis)
// taken straight across and straight down: x its bottom (m up the deck: the
// lowest of the lobes over it, its base or where its ellipsoid's underside
// is over that; where none is over it, the nearest's base), y how high
// its top stands over that (the highest lobe's; 0 none), z how far `up` is
// inside the lobes' outline across (m, by the lobe's shorter half-axis over
// its width: a flat lobe's walls as soft as cloudLobeIn's; negative
// outside, by at least that: a lobe's outline its base where its middle is
// under it, else its widest), w how far a point is inside under the top a
// metre under it (the highest lobe's, likewise). The 5 x 5 cells about it
// are searched, so a heap past them is more than a cell off.
vec4 cloudHeapColumn(vec3 up) {
#ifndef CLOUD_HEAPS
    return vec4(0.0, 0.0, -1e5, 1.0);
#else
    if (cloudHeapSide() <= 0.0) return vec4(0.0, 0.0, -1e5, 1.0);
    vec3 fe;
    ivec2 c0;
    float wall;
    cloudHeapAbout(up, fe, c0, wall);
    float rb = clouds.shell.x;
    float dIn = -(1.0 + wall) * cloudHeapSide(), zNear = 0.0;
    float zb = 1e9, top = -1e9, rateTop = 1.0;
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
            float m = mid / hh, least = min(r, hh);
            float rf = mid < 0.0 ? r * sqrt(max(1.0 - m * m, 0.0)) : r;
            float d = (rf - rho) * least / r;
            if (d > dIn) {
                dIn = d;
                zNear = base;
            }
            float q = rho / r;
            if (q >= 1.0) continue;
            float s = sqrt(1.0 - q * q);
            float t = mid + hh * s;   // (its top over its base)
            if (t <= 0.0) continue;
            zb = min(zb, base + max(mid - hh * s, 0.0));
            if (base + t > top) {
                top = base + t;
                rateTop = max(least / hh * s, 0.02);
            }
        }
    }
    if (top < -1e8) return vec4(zNear, 0.0, dIn, 1.0);
    return vec4(zb, top - zb, dIn, rateTop);
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
    float wall;
    cloudHeapAbout(up, fe, c0, wall);
    // (Lobes reach a cell from their middle, so a heap of a cell past the
    // 3 x 3 about the point's is at least its nearest wall's distance away.)
    float best = -wall * cloudHeapSide();
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

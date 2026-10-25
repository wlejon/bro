// The volume about the eye (sky/clouds_layout.h's CL_VOL_*; vulkan/sky/
// scene_cloud_volume.h): its frame, its levels' windows, its lookups.
//
// The includer has included sky/scene_cloud_layer.glsl.

layout(set = CLOUD_SET, binding = CL_B_VOLUME) uniform sampler3D cloudVolMap[CL_VOL_LEVELS];

const ivec3 kCloudVolSide = ivec3(CL_VOL_SIDE, CL_VOL_SIDE, CL_VOL_DEPTH);

// Level k's cell (m).
float cloudVolCell(int k) { return clouds.volA.w * exp2(float(k)); }

// Body point `p` in the volume's frame (m): drifted, about the anchor.
vec3 cloudVolLocal(vec3 p) {
    vec3 d = cloudDrift(p) - clouds.volA.xyz;
    return vec3(dot(d, clouds.volE1.xyz), dot(d, clouds.volE2.xyz), dot(d, clouds.volN.xyz));
}
// A body-frame direction in the volume's frame.
vec3 cloudVolDir(vec3 d) { return vec3(dot(d, clouds.volE1.xyz), dot(d, clouds.volE2.xyz), dot(d, clouds.volN.xyz)); }
// The volume's point `v` as a body point now.
vec3 cloudVolBody(vec3 v) {
    vec3 pd = clouds.volA.xyz + clouds.volE1.xyz * v.x + clouds.volE2.xyz * v.y + clouds.volN.xyz * v.z;
    return pd + clouds.wind.xyz * clouds.shell.w;
}

// Storage index `i` of a window about cell `c` along an axis of `n`
// cells: the cell it holds (n a power of two, so the mask is the modulus
// of a negative difference too).
ivec3 cloudVolCellOf(ivec3 i, ivec3 c) {
    ivec3 lo = c - kCloudVolSide / 2;
    return lo + ((i - lo) & (kCloudVolSide - 1));
}

// Whether level `k`'s window holds point `v`, CL_VOL_MARGIN cells inside
// its edges (a filtered lookup reads only cells it holds).
bool cloudVolHolds(vec3 v, int k) {
    if (clouds.volLevel[k].w < 0.5) return false;
    vec3 g = floor(v / cloudVolCell(k)) - clouds.volLevel[k].xyz;
    vec3 h = vec3(kCloudVolSide / 2 - CL_VOL_MARGIN);
    return all(greaterThanEqual(g, -h)) && all(lessThan(g, h));
}

// The finest level whose cell is `foot` or wider and whose window holds
// `v` (CL_VOL_LEVELS: none does).
int cloudVolLevel(vec3 v, float foot) {
    int k0 = int(max(ceil(log2(max(foot, 1e-3) / clouds.volA.w)), 0.0));
    for (int k = k0; k < CL_VOL_LEVELS; ++k)
        if (cloudVolHolds(v, k)) return k;
    return CL_VOL_LEVELS;
}

// Level `k` at `v`, trilinear (x the extinction: the distance and the
// occupancy only by cloudVolTexel). (A sampler of the
// array by a constant index: an index that varies by pixel is not
// dynamically uniform.)
vec4 cloudVolSample(int k, vec3 v) {
    vec3 uvw = v / (cloudVolCell(k) * vec3(kCloudVolSide));
    if (k == 0) return textureLod(cloudVolMap[0], uvw, 0.0);
    if (k == 1) return textureLod(cloudVolMap[1], uvw, 0.0);
    if (k == 2) return textureLod(cloudVolMap[2], uvw, 0.0);
    if (k == 3) return textureLod(cloudVolMap[3], uvw, 0.0);
    return textureLod(cloudVolMap[4], uvw, 0.0);
}
// The texel of level `k`'s cell that holds `v`.
vec4 cloudVolTexel(int k, vec3 v) {
    ivec3 i = ivec3(floor(v / cloudVolCell(k))) & (kCloudVolSide - 1);
    if (k == 0) return texelFetch(cloudVolMap[0], i, 0);
    if (k == 1) return texelFetch(cloudVolMap[1], i, 0);
    if (k == 2) return texelFetch(cloudVolMap[2], i, 0);
    if (k == 3) return texelFetch(cloudVolMap[3], i, 0);
    return texelFetch(cloudVolMap[4], i, 0);
}
// How far (m) a march at `v` (level `k`'s cell) may go sure to meet no
// cloud the forms draw at any footprint up to the cell's: the cell's
// distance less the slack its age leaves.
float cloudVolFree(int k, vec3 v) { return cloudVolTexel(k, v).y - clouds.volE1.w; }

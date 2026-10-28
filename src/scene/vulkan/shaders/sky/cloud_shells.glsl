// A heap drawn as a shell (sky/clouds_layout.h's CL_SHELL_*; sky/
// cloud_shells.vert, .frag): its record as the plan wrote it and the sort
// laid it out, its frame, the box about its lobes and a ray through them.
// Included after sky/scene_cloud_layer.glsl (set CLOUD_SET).

#include "sky/cloud_heaps.glsl"

layout(std430, set = CLOUD_SET, binding = CL_B_SHELLS) readonly buffer Shells {
    uint head[CL_SHELL_HEAD];
    vec4 rec[];
} shells;

struct ClShell {
    vec3 centre;    // its cell's middle at the deck's bottom (camera-relative, air space's axes)
    float dist;     // its distance (m)
    vec3 east;      // the plane tangent there
    vec3 north;
    vec3 up;
    float own;      // how much of it is drawn as itself, not as the heaps' share (1 - cloudHeapsFar)
    vec4 lobe[CL_HEAP_LOBES];
    vec4 middle;
    vec4 base;
};

ClShell clShell(int i) {
    uint o = uint(i) * uint(CL_SHELL_VEC4S);
    ClShell h;
    vec4 r0 = shells.rec[o];
    h.centre = r0.xyz;
    h.dist = r0.w;
    h.east = shells.rec[o + 1u].xyz;
    h.own = shells.rec[o + 1u].w;
    h.north = shells.rec[o + 2u].xyz;
    h.up = cross(h.east, h.north);
    for (int l = 0; l < CL_HEAP_LOBES; ++l) h.lobe[l] = shells.rec[o + 3u + uint(l)];
    h.middle = shells.rec[o + 7u];
    h.base = shells.rec[o + 8u];
    return h;
}

// How much of a heap is drawn as its shell: none where it begins to be
// handed over, all where it is wholly (the march's heaps, and the layer's
// tops, the other way across the same distances: clouds.frag's
// clHeapsKeep); as much of that as it is itself, not the heaps' share (the
// march's and the layer's where it is a few pixels across or less).
float clShellWeight(ClShell h) { return smoothstep(clouds.shellEye.w, clouds.shellParams.x, h.dist) * h.own; }

// How far out of its lobes' outlines a heap seen from `dist` may reach (m):
// the skin's most out, and half the surface's blur over a pixel there.
float clShellGrow(float dist) {
    return (0.55 * CLOUD_SKIN + 0.5 * cloudBlur(clouds.params.y * dist)) * clouds.forms.w;
}

// A lobe's ellipsoid (its middle and radii, the heap's frame).
void clShellLobe(ClShell h, int l, out vec3 c, out vec3 r) {
    c = vec3(h.lobe[l].xy, h.base[l] + h.middle[l]);
    r = vec3(max(h.lobe[l].zz, vec2(1.0)), max(h.lobe[l].w, 1.0));
}

// The box about its lobes (the heap's frame), `margin` metres out.
void clShellBox(ClShell h, float margin, out vec3 lo, out vec3 hi) {
    lo = vec3(1e9);
    hi = vec3(-1e9);
    for (int l = 0; l < CL_HEAP_LOBES; ++l) {
        if (h.lobe[l].w <= 0.0) continue;
        vec3 c, r;
        clShellLobe(h, l, c, r);
        lo = min(lo, vec3(c.xy - r.xy, h.base[l]));
        hi = max(hi, c + r);
    }
    lo -= margin;
    hi += margin;
}

// Where ray `o` + t `d` (the heap's frame) is within lobe `l`, from `t0` to
// `t1` (t1 <= t0: never): its half-ellipsoid, cut at its base. `edge` the
// ray's nearest approach to the ellipsoid's middle in its radii, 1 at its
// outline.
bool clShellThrough(ClShell h, int l, vec3 o, vec3 d, out float t0, out float t1, out float edge) {
    t0 = t1 = 0.0;
    edge = 2.0;
    if (h.lobe[l].w <= 0.0) return false;
    vec3 c, r;
    clShellLobe(h, l, c, r);
    vec3 os = (o - c) / r, ds = d / r;
    float a = dot(ds, ds), b = dot(os, ds), cc = dot(os, os) - 1.0;
    float disc = b * b - a * cc;
    edge = sqrt(max(dot(os, os) - b * b / a, 0.0));
    if (disc <= 0.0) return false;
    float s = sqrt(disc);
    t0 = (-b - s) / a;
    t1 = (-b + s) / a;
    float zb = h.base[l];
    if (d.z > 1e-6) t0 = max(t0, (zb - o.z) / d.z);
    else if (d.z < -1e-6) t1 = min(t1, (zb - o.z) / d.z);
    else if (o.z < zb) return false;
    return t1 > t0;
}

// The heap's column over frame point `v` (sky/cloud_heaps.glsl's
// cloudHeapColumn, of this heap's lobes alone: what the heaps' map holds,
// which the march reads): x its bottom, y its top's height over that, z how
// far inside its outline across, w its top's slope's cosine.
vec4 clShellColumn(ClShell h, vec2 v) {
    const float CL_HEAP_RATE_NEAR = 100.0, CL_HEAP_BOTTOM_NEAR = 200.0;
    float dIn = -1e5, zNear = 0.0, cNear = 1.0;
    float bm = 1e9, botW = 0.0, botSum = 0.0;
    float top = -1e9, rateW = 0.0, rateSum = 0.0;
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
            zNear = base + max(mid, 0.0);
            float sm = max(-m, 0.0);
            cNear = sm > 0.0 ? (sm / hh) * inversesqrt((1.0 - sm * sm) / (r * r) + sm * sm / (hh * hh)) : 0.0;
        }
        float q = rho / r;
        if (q >= 1.0) continue;
        float s = sqrt(1.0 - q * q);
        float t = mid + hh * s;
        if (t <= 0.0) continue;
        float bot = base + max(mid - hh * s, 0.0);
        if (bot < bm) {
            botSum *= exp((bot - bm) / CL_HEAP_BOTTOM_NEAR);
            bm = bot;
        }
        float s2 = s * s, wb = s2 * s2;
        botW += wb;
        botSum += wb * exp((bm - bot) / CL_HEAP_BOTTOM_NEAR);
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
    if (top < -1e8) return vec4(zNear, 0.0, dIn, cNear);
    float zb = min(bm - CL_HEAP_BOTTOM_NEAR * log(max(botSum, 1e-30) / max(botW, 1e-30)), top);
    return vec4(zb, top - zb, dIn, rateSum / max(rateW, 1e-6));
}

// The heap's frame's view of camera-relative point `p` and direction `v`.
vec3 clShellPoint(ClShell h, vec3 p) { vec3 q = p - h.centre; return vec3(dot(q, h.east), dot(q, h.north), dot(q, h.up)); }
vec3 clShellDir(ClShell h, vec3 v) { return vec3(dot(v, h.east), dot(v, h.north), dot(v, h.up)); }

// Grass: dense procedural blades, the GPU half of scene/grass_node.h. Needs
// scene_camera.glsl (the camera, the wind field, the hashes and noise).
//
// No blade has any data of its own. A ring of cells around the camera is
// drawn as one instanced draw, and each instance finds its blade from its
// index: the cell it stands in (the draw's rectangle of absolute cells), a
// hashed spot in that cell, the ground there (the node's field texture), and
// the clump noise. Everything after is per vertex:
//
//   - the blade is an arc whose bend is the wind (the steady lean, its own
//     flutter, and the travelling gust bands of sceneWindGustAt) plus the
//     influence map (movers push blades aside and flatten them);
//   - it is lit mostly as the ground it covers: its normal leans toward the
//     ground's (vegShade.xyz), so a meadow reads as one surface;
//   - its colour runs from lush to sun-dried with the field's dry mask, dark
//     at the root and lifted where a gust bends it.
//
// A blade the draw does not want (outside its ring, off screen, thinned by
// the density) is culled at every vertex by the same instance-only test, so
// no triangle of it survives.

layout(set = 4, binding = 0) uniform GrassUBO {
    vec4 cells;      // x, y: the rectangle's first cell (absolute column, row); z: its width in cells; w: cell size (m)
    vec4 ring;       // x: inner radius, y: outer radius, z: the cross-fade band at the inner edge, w: at the outer (m)
    vec4 field;      // xy: world x, z of texel (0, 0)'s centre; z: texel spacing (m); w: unused
    vec4 fieldSize;  // cols, rows, 1 / cols, 1 / rows
    vec4 blade;      // x: mean height (m), y: width (m), z: min ground normal y, w: wind response
    vec4 lush;       // rgb lush colour, w: ground-normal weight
    vec4 dry;        // rgb sun-dried colour, w: clump frequency (1/m)
    vec4 look;       // x: root shade, y: gust sheen, z: height variety, w: width growth per metre of distance
    vec4 push;       // xy: world x, z of the influence map's corner; z: its cell (m); w: cells across
    vec4 pushRate;   // x: bend recovery (1/s), y: trample recovery (1/s), zw: unused
} grass;

const float GRASS_REFERENCE_SPEED = 2.5;   // m/s: the steady wind blades flutter in as authored

// r: ground height, g: density (0..1), b: dryness (0 lush .. 1 dry), a: height multiplier.
layout(set = 4, binding = 1) uniform sampler2D grassField;
// rg: the bend (x, z) at stamp time, b: the trample then, a: the stamp time.
layout(set = 4, binding = 2) uniform sampler2D grassPush;

vec4 grassFieldAt(vec2 xz) {
    vec2 uv = ((xz - grass.field.xy) / grass.field.z + 0.5) * grass.fieldSize.zw;
    return textureLod(grassField, uv, 0.0);
}

bool grassInField(vec2 xz) {
    vec2 q = (xz - grass.field.xy) / grass.field.z;
    return q.x >= 0.0 && q.y >= 0.0 && q.x <= grass.fieldSize.x - 1.0 && q.y <= grass.fieldSize.y - 1.0;
}

vec3 grassGroundNormal(vec2 xz) {
    float e = grass.field.z;
    float hx = grassFieldAt(xz + vec2(e, 0.0)).r - grassFieldAt(xz - vec2(e, 0.0)).r;
    float hz = grassFieldAt(xz + vec2(0.0, e)).r - grassFieldAt(xz - vec2(0.0, e)).r;
    return normalize(vec3(-hx, 2.0 * e, -hz));
}

// The movers' push at xz: the bend now (decayed since it was stamped) and,
// in `trample`, how flat the grass there is.
vec2 grassInfluence(vec2 xz, out float trample) {
    trample = 0.0;
    vec2 q = (xz - grass.push.xy) / (grass.push.z * grass.push.w);
    if (q.x <= 0.0 || q.y <= 0.0 || q.x >= 1.0 || q.y >= 1.0) return vec2(0.0);
    vec4 p = textureLod(grassPush, q, 0.0);
    float age = camera.windParams.x - p.a;   // the stamps are on the wind clock
    if (p.a <= 0.0 || age < 0.0) return vec2(0.0);
    trample = max(p.b - age * grass.pushRate.y, 0.0);
    return p.rg * exp(-age * grass.pushRate.x);
}

struct GrassBlade {
    vec3 pos;      // camera-relative
    vec3 normal;   // the blade's own
    vec3 side;     // across the blade
    vec3 ground;   // the ground's normal under it
    vec3 color;    // linear, before the node colour
    float ao;      // 0.5 at the root .. 1 at the tip
};

// Blade `id` of the draw at blade-space vertex `v` (x across -0.5..0.5, y 0
// at the root .. 1 at the tip) with normal `vn`. False when the draw does not
// draw that blade.
bool grassBlade(int id, vec3 v, vec3 vn, out GrassBlade b) {
    b.pos = vec3(0.0);
    b.normal = vec3(0.0, 1.0, 0.0);
    b.side = vec3(1.0, 0.0, 0.0);
    b.ground = vec3(0.0, 1.0, 0.0);
    b.color = vec3(0.0);
    b.ao = 1.0;

    int W = max(int(grass.cells.z), 1);
    ivec2 c = ivec2(int(grass.cells.x) + id % W, int(grass.cells.y) + id / W);
    vec4 h = vec4(sceneWindHash(c), sceneWindHash(c + ivec2(71, 13)), sceneWindHash(c + ivec2(-35, 29)),
                  sceneWindHash(c + ivec2(17, -91)));
    float cell = grass.cells.w;
    vec2 xz = (vec2(c) + h.xy) * cell;
    vec3 eye = camera.eyeWorld.xyz;
    float d = length(xz - eye.xz);

    // The ring, handing over to its neighbours through a dithered band.
    float fin = grass.ring.x <= 0.0 ? 1.0 : smoothstep(grass.ring.x - grass.ring.z, grass.ring.x, d);
    float fout = 1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, d);
    if (h.w > fin || h.w > fout) return false;
    if (!grassInField(xz)) return false;
    vec4 f = grassFieldAt(xz);
    if (f.g <= 0.0 || f.a <= 0.0) return false;

    // Off screen: a generous box around the blade's middle.
    float H = grass.blade.x;
    vec4 cp = camera.viewProj * vec4(xz.x - eye.x, f.r + 0.6 * H - eye.y, xz.y - eye.z, 1.0);
    float m = cp.w * 0.15 + 2.0 * H + 0.5;
    if (cp.w < -2.0 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;

    // Clumps: thick and tall in some places, thin and short between.
    vec3 gn = grassGroundNormal(xz);
    float k = grass.dry.w;
    float clump = clamp((0.5 * sceneWindNoise(xz * k) + 0.25 * sceneWindNoise(xz * k * 2.03 + 17.3)) * 0.667 + 0.5,
                        0.0, 1.0);
    float minY = grass.blade.z;
    float dens = (0.35 + 0.9 * clump) * f.g * smoothstep(minY - 0.05, minY + 0.05, gn.y);
    if (h.z > dens) return false;

    float lush = 1.0 - clamp(f.b, 0.0, 1.0);
    float tall = smoothstep(0.35, 0.8, sceneWindNoise(xz * 0.045 + 3.0) * 0.5 + 0.5);
    float trample;
    vec2 pb = grassInfluence(xz, trample);
    float ht = H * mix(0.5, 1.4, clump * clump) * (0.6 + 0.8 * h.z) *
               mix(1.0, mix(0.8, 1.7, tall * lush), grass.look.z) * f.a;
    ht *= 1.0 - trample * 0.5;
    float wd = grass.blade.y * (1.0 + d * grass.look.w);
    float yaw = h.x * 6.2831853 + h.y;
    vec3 side = vec3(cos(yaw), 0.0, sin(yaw));

    // The wind: a lean downwind, the travelling gusts, each blade's flutter
    // and a rest lean of its own; then the movers' push.
    float gu = sceneWindGustAt(xz);
    float calm = clamp(camera.windField.z / GRASS_REFERENCE_SPEED, 0.0, 2.0);
    float spd = camera.windField.z + gu * 4.0;
    vec2 wdir = camera.windField.xy;
    float t = camera.windParams.x;
    float flut = sin(t * (2.2 + h.y * 2.5) + dot(xz, wdir) * 0.9 + h.x * 9.0) * (0.06 * calm + 0.12 * gu);
    vec2 B = wdir * (0.1 * min(calm, 1.0) + spd * 0.07 + gu * 0.35) * grass.blade.w +
             vec2(-side.z, side.x) * flut * grass.blade.w +
             vec2(cos(yaw * 3.0), sin(yaw * 3.0)) * 0.18 * (h.y - 0.3);
    B += pb * 1.6;
    float th = min(length(B), 1.35) + trample * 1.1 * h.z;
    th = min(th, 1.5);
    vec2 bd = normalize(B + 1e-4);

    // The arc: curvature grows toward the tip.
    float y = v.y;
    float a = th * y * (0.6 + 0.4 * y);
    float sa = sin(a), ca = cos(a);
    float along = th > 1e-3 ? (1.0 - ca) * y / max(a, 1e-3) : 0.0;
    float up = th > 1e-3 ? sa * y / max(a, 1e-3) : y;
    vec3 dir3 = vec3(bd.x, 0.0, bd.y);
    vec3 root = vec3(xz.x - eye.x, f.r - eye.y, xz.y - eye.z);
    b.pos = root + side * v.x * wd + (vec3(0.0, up, 0.0) + dir3 * along) * ht;
    vec3 tang = normalize(vec3(0.0, ca, 0.0) + dir3 * sa);
    b.normal = normalize(cross(side, tang) + side * vn.x * 0.6);
    b.side = side;
    b.ground = gn;

    // Colour: lush to dry by the mask and the clump, dark at the root, and
    // a gust lifts the sheen of the blades it bends.
    vec3 green = grass.lush.rgb * mix(0.85, 1.15, h.y);
    vec3 dryc = grass.dry.rgb * mix(0.88, 1.12, h.x);
    vec3 col = mix(dryc, green, clamp(lush * 1.3 - 0.15 + clump * 0.2, 0.0, 1.0));
    col = mix(col * grass.look.x, col * (1.0 + 0.2 * y), y);
    col *= 1.0 + gu * grass.look.y * y;
    b.color = col;
    b.ao = mix(0.5, 1.0, y);
    return true;
}

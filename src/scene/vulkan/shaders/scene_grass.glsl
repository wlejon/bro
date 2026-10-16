// Grass: dense procedural blades, the GPU half of scene/grass_node.h. Needs
// scene_camera.glsl (the camera, the wind field, the hashes and noise).
//
// No blade has any data of its own. A ring of cells around the camera is
// drawn as one instanced draw. An instance is a cell: its root is a hashed
// spot in the cell (the draw's rectangle of absolute cells), and it draws
// several blades (the ring mesh is that many copies of the blade, so the
// vertex index says which), each standing a hashed step from the root: a
// tuft fanning out near the camera, blades scattered over the cell further
// off. The ground there (the node's field texture) and the clump noise do the
// rest. Everything after is per vertex:
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
    vec4 cells;      // xyz: unused (the draw's cells are grassRect()); w: cell size (m)
    vec4 ring;       // x: inner radius, y: outer radius, z: the cross-fade band at the inner edge, w: at the outer (m)
    vec4 field;      // xy: world x, z of texel (0, 0)'s centre; z: texel spacing (m); w: unused
    vec4 fieldSize;  // cols, rows, 1 / cols, 1 / rows
    vec4 blade;      // x: mean height (m), y: width (m), z: min ground normal y, w: wind response
    vec4 lush;       // rgb lush colour, w: ground-normal weight
    vec4 dry;        // rgb sun-dried colour, w: clump frequency (1/m)
    vec4 look;       // x: root shade, y: gust sheen, z: height variety, w: width growth per metre of distance
    vec4 push;       // xy: world x, z of the influence map's corner; z: its cell (m); w: cells across
    vec4 pushRate;   // x: bend recovery (1/s), y: trample recovery (1/s), zw: unused
    vec4 shape;      // x: blades an instance, y: vertices a blade, z: spread from the root (m), w: a tuft's fan (rad)
    vec4 lod;        // x: thinning starts (m), y: the fewest blades kept (share), z: the share shadow casters keep, w: unused
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

// The movers' push at xz: the bend now (decayed since it was stamped); in
// `trample`, how flat the grass there is, and in `lie`, the way flattened
// blades lie (the push's direction when it was stamped).
vec2 grassInfluence(vec2 xz, out float trample, out vec2 lie) {
    trample = 0.0;
    lie = vec2(0.0);
    vec2 q = (xz - grass.push.xy) / (grass.push.z * grass.push.w);
    if (q.x <= 0.0 || q.y <= 0.0 || q.x >= 1.0 || q.y >= 1.0) return vec2(0.0);
    vec4 p = textureLod(grassPush, q, 0.0);
    float age = camera.windParams.x - p.a;   // the stamps are on the wind clock
    if (p.a <= 0.0 || age < 0.0) return vec2(0.0);
    trample = max(p.b - age * grass.pushRate.y, 0.0);
    lie = p.rg;
    return p.rg * exp(-age * grass.pushRate.x);
}

struct GrassBlade {
    vec3 pos;      // camera-relative
    vec3 normal;   // the blade's own
    vec3 side;     // across the blade
    vec3 ground;   // the ground's normal under it
    vec3 color;    // linear, before the node colour
    float ao;      // 0.5 at the root .. 1 at the tip
    float bulk;    // how much of its shading follows the ground's normal
    float far;     // 0 near .. 1: how far its translucency has faded
};

// The draw's rectangle of cells: its first column and row and its width in
// cells, in the model matrix's first row (scene_mesh_drawer_grass.cpp: a
// draw a run of the tiles in view).
vec3 grassRect() {
#ifdef GRASS_SHADOW
    return push.modelRows[0].xyz;
#else
    return vec3(push.model[0][0], push.model[1][0], push.model[2][0]);
#endif
}

// A hash of cell `c` and slot `k` in [0, 1): a blade's own draws.
float grassHash(ivec2 c, int k) {
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u + uint(k) * 2246822519u;
    h = (h ^ (h >> 15)) * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) / 4294967296.0;
}

// Instance `id` of the draw at vertex `vid` of its ring mesh, blade-space
// vertex `v` (x across -0.5..0.5, y 0 at the root .. 1 at the tip) with
// normal `vn`. False when the draw does not draw that blade.
bool grassBlade(int id, int vid, vec3 v, vec3 vn, out GrassBlade b) {
    b.pos = vec3(0.0);
    b.normal = vec3(0.0, 1.0, 0.0);
    b.side = vec3(1.0, 0.0, 0.0);
    b.ground = vec3(0.0, 1.0, 0.0);
    b.color = vec3(0.0);
    b.ao = 1.0;
    b.bulk = grass.lush.w;
    b.far = 0.0;

    vec3 rect = grassRect();
    int W = max(int(rect.z), 1);
    ivec2 c = ivec2(int(rect.x) + id % W, int(rect.y) + id / W);
    vec4 h = vec4(sceneWindHash(c), sceneWindHash(c + ivec2(71, 13)), sceneWindHash(c + ivec2(-35, 29)),
                  sceneWindHash(c + ivec2(17, -91)));
    float cell = grass.cells.w;
    vec2 root = (vec2(c) + h.xy) * cell;
    vec3 eye = camera.eyeWorld.xyz;
    float d = length(root - eye.xz);

    // Thinning, first since it is the cheapest test: past lod.x the instance
    // keeps fewer of its blades, widened to keep the cover; shadow casters
    // keep fewer again.
    int j = vid / max(int(grass.shape.y + 0.5), 1);
    float keep = clamp(grass.lod.x / max(d, 1e-3), grass.lod.y, 1.0);
#ifdef GRASS_SHADOW
    keep *= grass.lod.z;
#endif
    float kept = max(grass.shape.x * keep, 1.0);
    if (float(j) + grassHash(c, 4096 + j) * 0.999 >= kept + 0.5) return false;

    // The ring, handing over to its neighbours through a dithered band (a
    // whole instance at a time).
    float fin = grass.ring.x <= 0.0 ? 1.0 : smoothstep(grass.ring.x - grass.ring.z, grass.ring.x, d);
    float fout = 1.0 - smoothstep(grass.ring.y - grass.ring.w, grass.ring.y, d);
    if (h.w > fin || h.w > fout) return false;
    if (!grassInField(root)) return false;
    vec4 f = grassFieldAt(root);
    if (f.g <= 0.0 || f.a <= 0.0) return false;

    // Off screen: a generous box around the cell's middle.
    float H = grass.blade.x;
    vec4 cp = camera.viewProj * vec4(root.x - eye.x, f.r + 0.6 * H - eye.y, root.y - eye.z, 1.0);
    float m = cp.w * 0.15 + 2.0 * H + 0.5 + grass.shape.z;
    if (cp.w < -2.0 || cp.x < -cp.w - m || cp.x > cp.w + m || cp.y < -cp.w - m || cp.y > cp.w + m) return false;

    // Clumps: thick and tall in some places, thin and short between. The
    // density thins blade by blade, so a thin patch is sparse tufts.
    vec3 gn = grassGroundNormal(root);
    float k = grass.dry.w;
    float clump = clamp((0.5 * sceneWindNoise(root * k) + 0.25 * sceneWindNoise(root * k * 2.03 + 17.3)) * 0.667 + 0.5,
                        0.0, 1.0);
    float minY = grass.blade.z;
    float dens = (0.35 + 0.9 * clump) * f.g * smoothstep(minY - 0.05, minY + 0.05, gn.y);
    vec4 hb =vec4(grassHash(c, j * 4), grassHash(c, j * 4 + 1), grassHash(c, j * 4 + 2), grassHash(c, j * 4 + 3));
    if (hb.w > dens) return false;

    // The blade's spot: a step from the root, uniform over a disc of the
    // ring's spread (a tuft near, the cell further off).
    float rr = sqrt(hb.x);
    float ra = hb.y * 6.2831853;
    vec2 out2 = vec2(cos(ra), sin(ra));
    vec2 xz = root + out2 * (rr * grass.shape.z);
    float y0 = grassInField(xz) ? grassFieldAt(xz).r : f.r;

    float lush = 1.0 - clamp(f.b, 0.0, 1.0);
    float tall = smoothstep(0.35, 0.8, sceneWindNoise(root * 0.045 + 3.0) * 0.5 + 0.5);
    float trample;
    vec2 lie;
    vec2 pb = grassInfluence(xz, trample, lie);
    trample = min(trample, 1.0);
    float ht = H * mix(0.5, 1.4, clump * clump) * (0.85 + 0.3 * h.z) * (0.6 + 0.8 * hb.z) *
               mix(1.0, mix(0.8, 1.7, tall * lush), grass.look.z) * f.a;
    ht *= 1.0 - trample * 0.25;
    // Width grows with distance, and never falls under about two thirds of a
    // pixel: a thinner blade only flickers in and out of the samples.
    float px = 2.0 / max(abs(camera.proj[1][1]) * camera.viewport.y, 1.0);
    float dv = length(vec3(xz.x, y0, xz.y) - eye);
    float wd = max(grass.blade.y * (1.0 + d * grass.look.w) * (grass.shape.x / kept), dv * px * 0.65);
    // In a tuft a blade's face turns out from the root; elsewhere at random.
    float hr = fract(hb.x * 13.73 + hb.z * 5.17);
    float yaw = grass.shape.w > 0.0 ? ra + 1.5707963 + (hr - 0.5) * 1.2 : hr * 6.2831853 + hb.y;
    vec3 side = vec3(cos(yaw), 0.0, sin(yaw));

    // The wind: a lean downwind, the travelling gusts, each blade's flutter
    // and a rest lean of its own (out from a tuft's root, the outer blades
    // most); then the movers' push.
    float gu = sceneWindGustAt(root);
    float calm = clamp(camera.windField.z / GRASS_REFERENCE_SPEED, 0.0, 2.0);
    float spd = camera.windField.z + gu * 4.0;
    vec2 wdir = camera.windField.xy;
    float t = camera.windParams.x;
    float flut = sin(t * (2.2 + hb.y * 2.5) + dot(xz, wdir) * 0.9 + hr * 9.0) * (0.06 * calm + 0.12 * gu);
    vec2 B = wdir * (0.1 * min(calm, 1.0) + spd * 0.07 + gu * 0.35) * grass.blade.w +
             vec2(-side.z, side.x) * flut * grass.blade.w +
             vec2(cos(yaw * 3.0), sin(yaw * 3.0)) * 0.18 * (hb.z - 0.3) * (1.0 - min(grass.shape.w * 2.0, 0.6)) +
             out2 * grass.shape.w * (0.3 + 0.7 * rr) * (1.3 - 0.6 * hb.z);
    B += pb * 1.6;
    // Flattened grass lies the way it was pushed, most blades nearly flat,
    // a few springing back first.
    vec2 lieDir = length(lie) > 1e-3 ? normalize(lie) : normalize(B + 1e-4);
    float flattened = trample * mix(0.75, 1.0, hb.z);
    B = mix(B, lieDir * length(B), flattened);
    float r0 = flattened * 1.2;   // a flattened blade leaves the ground leaning
    float th = min(length(B), 1.5 - r0);
    vec2 bd = normalize(B + 1e-4);

    // The arc: a lean r0 at the root, curvature growing toward the tip; the
    // blade's angle from vertical at height fraction s is r0 + a(s).
    float y = v.y;
    float a = th * y * (0.6 + 0.4 * y);
    float ang = r0 + a;
    float sa = sin(ang), ca = cos(ang);
    float along = a > 1e-3 ? y * (cos(r0) - ca) / a : y * sin(r0);
    float up = a > 1e-3 ? y * (sa - sin(r0)) / a : y * cos(r0);
    vec3 dir3 = vec3(bd.x, 0.0, bd.y);
    vec3 base = vec3(xz.x - eye.x, y0 - eye.y, xz.y - eye.z);
    b.pos = base + side * v.x * wd + (vec3(0.0, up, 0.0) + dir3 * along) * ht;
    vec3 tang = normalize(vec3(0.0, ca, 0.0) + dir3 * sa);
    b.normal = normalize(cross(side, tang) + side * vn.x * 0.6);
    b.side = side;
    b.ground = gn;

    // Colour: lush to dry by the mask and the clump, dark at the root, and
    // a gust lifts the sheen of the blades it bends.
    // Each blade its own value and a little hue: the streaks that make a
    // sward read as blades rather than as a painted surface.
    float tone = mix(0.7, 1.25, fract(hb.y * 7.31 + hb.x));
    float hue = fract(hb.w * 5.31 + hr);
    vec3 green = grass.lush.rgb * mix(vec3(0.85, 0.95, 1.1), vec3(1.15, 1.05, 0.8), hue) * tone;
    vec3 dryc = grass.dry.rgb * mix(0.85, 1.15, hr) * tone;
    vec3 col = mix(dryc, green, clamp(lush * 1.3 - 0.15 + clump * 0.2, 0.0, 1.0));
    // Far off, a blade is a pixel or two: its dark root would leave only the
    // tips showing (a field of dots), so the root shade fades with distance
    // and a far blade reads as the colour of the turf it stands for.
    float far = smoothstep(12.0, 70.0, d) * 0.75;
    col = mix(col * mix(grass.look.x, 1.0, far), col * (1.0 + 0.2 * y), y);
    col *= 1.0 + gu * grass.look.y * y;
    col *= 1.0 + 0.6 * flattened;   // a flattened lane shows the blades' paler length
    b.color = col;    b.ao = mix(mix(0.5, 1.0, y), 1.0, far);
    b.bulk = mix(grass.lush.w, 0.95, far);   // and is lit as the ground,
    b.far = smoothstep(8.0, 40.0, d);         // not glowing against the light
    return true;
}

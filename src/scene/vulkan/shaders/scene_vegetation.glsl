// Vegetation sway: plants bending in the scene's wind field the way trees
// move. Needs scene_camera.glsl (the wind field and the clock).
//
// Every vertex (or instance, or procedural segment) carries four vec4s in
// node space, the layout bro.flora's emitters write (docs/scene-nodes-api.js
// "Vegetation", bromesh::MeshData::vegetation):
//
//   v0 (root.xyz, height)        the plant's base and height
//   v1 (crown.xyz, crownRadius)  the leafy volume; radius 0: no bulk normal
//   v2 (anchor.xyz, weight)      where a leaf hangs; branch weight 0 trunk .. 1 twig tip
//   v3 (axis.xyz, phase)         a leaf's stem direction (zero on wood); the limb's phase
//
// The motion, all evaluated per vertex with nothing on the CPU:
//   - the trunk leans into the gust at the plant's root, growing with
//     (height above the root / plant height)^2, with a slow sway of its own;
//   - each limb bobs and swings on its own phase, scaled by weight^2, so the
//     twigs move most and a limb leaves the trunk without tearing;
//   - a leaf moves rigidly with its twig at its anchor, then flutters: it
//     twists about its stem (showing its underside) and pitches, harder in
//     a gust.
// The gust (sceneWind's w) is the band passing over the plant, so a band
// travelling downwind visibly crosses a grove tree by tree.

const float VEG_REFERENCE_SPEED = 2.5;   // m/s: the default breeze moves plants as authored

// Rotation by `ang` about unit `axis` (Rodrigues), as a matrix.
mat3 vegAxisAngle(vec3 axis, float ang) {
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    vec3 a = axis;
    return mat3(t * a.x * a.x + c,       t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c,       t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}

// Sway one vertex. `pos` is its camera-relative rest position, bent in place;
// `rot` receives the rotation its normal and tangent take (a leaf's flutter,
// identity on wood). Returns the shading varying mesh.frag reads under
// SCENE_VEGETATION: xyz the bulk normal times its weight (zero on wood or
// without a crown), w the ambient occlusion, negative on a leaf.
vec4 vegetationSway(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3, inout vec3 pos, out mat3 rot) {
    mat3 M3 = mat3(model);
    float scale = length(M3[1]);
    vec3 root = (model * vec4(v0.xyz, 1.0)).xyz;
    float H = max(v0.w * scale, 0.05);
    vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
    bool leaf = dot(v3.xyz, v3.xyz) > 0.25;
    rot = mat3(1.0);

    // The wind at the plant: the gust band over it now, the steady speed,
    // the drifting direction.
    vec4 wind = sceneWind(root);
    float gu = wind.w;
    vec2 wd = camera.windField.xy;
    float calm = clamp(camera.windField.z / VEG_REFERENCE_SPEED, 0.0, 3.0);
    float spd = camera.windField.z + gu * 4.0;
    float t = camera.windParams.x;
    vec3 rootAbs = root + camera.eyeWorld.xyz;
    float tree = sceneWindHash(ivec2(floor(rootAbs.xz * 8.0)));

    // The trunk: a lean into the wind plus a slow sway of the whole plant,
    // both growing with height squared; the top drops as it leans.
    vec3 rest = leaf ? anchor : pos;
    float h = clamp((rest.y - root.y) / H, 0.0, 1.3);
    float osc = sin(t * (0.8 + 0.25 * tree) + rootAbs.x * 0.37 + rootAbs.z * 0.21);
    float osc2 = sin(t * 1.9 + rootAbs.x * 0.11 - rootAbs.z * 0.3);
    vec2 trunk = (wd * (spd * 0.006 + gu * 0.03 + osc * 0.008 * (0.6 * calm + gu)) +
                  vec2(-wd.y, wd.x) * osc2 * 0.004 * (0.5 * calm + gu)) * H * h * h;

    // The limb: pushed downwind, bobbing and swinging on its own phase,
    // weighted by branch weight squared. A small plant's limbs reach less.
    float ph = v3.w, bw = clamp(v2.w, 0.0, 1.0);
    if (!leaf && v1.w < 0.0) {
        // A wood segment record (one record over a whole segment, an
        // instanced cylinder): v2 holds its base and the weight there, v1 its
        // tip and -(1 + the weight there); each vertex takes the weight at
        // its own point along it, so the tip bends as its child's base does.
        vec3 tip = (model * vec4(v1.xyz, 1.0)).xyz;
        vec3 d = tip - anchor;
        float dd = dot(d, d);
        float along = dd > 1e-12 ? clamp(dot(pos - anchor, d) / dd, 0.0, 1.0) : 1.0;
        bw = mix(bw, clamp(-v1.w - 1.0, 0.0, 1.0), along);
    }
    float bo = sin(t * (1.6 + ph * 1.1) + ph * 40.0);
    float bs = sin(t * (1.1 + ph * 0.7) + ph * 23.0);
    // A tall tree's limbs are long, and a long limb's tip sweeps further.
    vec3 limb = vec3(wd.x, 0.0, wd.y) * (0.05 * calm + 0.16 * gu + 0.06 * bs * calm) +
                vec3(0.0, 0.07, 0.0) * bo * (0.4 * calm + gu) +
                vec3(-wd.y, 0.0, wd.x) * bs * 0.05 * (0.6 * calm + gu);
    limb *= bw * bw * (0.5 + 0.11 * H) * clamp(H / 3.0, 0.15, 1.0);

    vec3 bend = vec3(trunk.x, -dot(trunk, trunk) * 0.1 / max(H, 1.0), trunk.y) + limb;

    if (!leaf) {
        pos += bend;
        // Bark darkens toward the ground.
        return vec4(0.0, 0.0, 0.0, mix(0.6, 1.0, clamp(h, 0.0, 1.0)));
    }

    // The leaf: carried with its twig at the anchor, then fluttering about
    // its stem and pitching, each leaf at its own rate.
    vec3 axis = normalize(M3 * v3.xyz);
    float sd = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719))) * 43758.5453) * 97.0;
    float flutter = sin(t * (4.0 + fract(sd) * 6.0) + sd * 6.2831) * (0.12 * calm + 0.55 * gu);
    float pitch = sin(t * (3.1 + fract(sd * 3.0) * 4.0) + sd * 2.0) * (0.08 * calm + 0.4 * gu) + gu * 0.5;
    vec3 side = cross(axis, vec3(0.0, 1.0, 0.0));
    side = dot(side, side) < 1e-6 ? vec3(1.0, 0.0, 0.0) : normalize(side);
    rot = vegAxisAngle(side, pitch) * vegAxisAngle(axis, flutter);
    pos = anchor + bend + rot * (pos - anchor);

    // The crown as one form: the leaf's normal leans toward the crown's
    // centre-out direction, and leaves deep inside take less sky.
    if (v1.w <= 0.0) return vec4(0.0, 0.0, 0.0, -1.0);
    vec3 crown = (model * vec4(v1.xyz, 1.0)).xyz;
    vec3 rc = (anchor - crown) / max(v1.w * scale, 0.5);
    vec3 bulk = normalize(rc + vec3(0.0, 0.25, 0.0));
    float depth = clamp(length(rc), 0.0, 1.2);
    float ao = clamp(0.25 + 0.85 * depth * depth, 0.25, 1.0) * mix(0.75, 1.0, clamp(rc.y * 0.5 + 0.6, 0.0, 1.0));
    return vec4(bulk * 0.6, -ao);
}

// The colour a plant's part takes on top of its material, from the same
// records: rgb multiplies the base colour (the vertex shaders fold it into
// the instance tint). A tree is never one flat hue:
//   - each plant leans warm (yellow-green) or cool (blue-green), hashed from
//     its root;
//   - each limb is lighter or darker (its phase), and each leaf a little;
//   - leaves deep in the crown are darker and cooler, the rim lighter, so a
//     crown is dark inside and lit at its edge.
// Bark varies by plant, and darkens toward the ground.
vec3 vegetationTint(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3) {
    float scale = length(mat3(model)[1]);
    vec3 root = (model * vec4(v0.xyz, 1.0)).xyz;
    vec3 rootAbs = root + camera.eyeWorld.xyz;
    float tree = sceneWindHash(ivec2(floor(rootAbs.xz * 8.0)) + ivec2(17, 3));
    float limb = fract(v3.w * 7.31 + tree * 3.7);
    bool leaf = dot(v3.xyz, v3.xyz) > 0.25;
    if (!leaf) return vec3(0.82 + 0.3 * tree, 0.8 + 0.28 * tree, 0.78 + 0.24 * tree);

    float each = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    vec3 c = mix(vec3(0.84, 0.97, 1.08), vec3(1.12, 1.04, 0.76), tree);
    c *= 0.8 + 0.4 * limb;
    c *= 0.9 + 0.2 * each;
    if (v1.w > 0.0) {
        vec3 crown = (model * vec4(v1.xyz, 1.0)).xyz;
        vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
        float depth = length(anchor - crown) / max(v1.w * scale, 0.5);
        c *= mix(vec3(0.5, 0.62, 0.68), vec3(1.0), smoothstep(0.25, 1.0, depth));
        c *= 1.0 + 0.15 * smoothstep(0.85, 1.15, depth);
    }
    return c;
}

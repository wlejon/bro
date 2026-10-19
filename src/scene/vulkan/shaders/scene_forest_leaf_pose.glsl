// A forest leaf posed (scene_forest.glsl's ForestPose): everything a leaf's
// vertices share, worked out once a leaf by forest_leaf_pose.comp, or by
// each vertex in mesh_forest_leaf.vert's FOREST_LEAF_INLINE build (the
// leaves past the pose buffer's room: a forced Leaves tier over a whole
// forest). Where forestLeafFrame places it (a broad leaf at its node on its
// shoot, scene_forest_shoot.glsl), its widening, its sway (the wood's pose
// where it hangs, its spray's and shoot's bending, its own turn and
// flutter about its petiole; its fold is the vertex's, by the side of the
// stem it is on), its crown shading and tint, a cluster's lobe. Needs
// scene_camera.glsl, scene_mesh_push.glsl (the draw's flags say cluster),
// scene_procedural.glsl, scene_vegetation.glsl, scene_forest.glsl and
// scene_forest_shoot.glsl, with forestApplyLook() called first.
// forestCasterPoseLeaf is the same for a leaf's shadow caster (the one a
// shadow vertex builds from scene_shadow_push.glsl, which has no colour pose).

#ifndef VEG_SHADOW_CASTER
ForestPose forestPoseLeaf(ForestEntry e, int leaf) {
    ForestPose p;
    p.alive = false;
    // Widened, and shrinking away over the tail of the kept run.
    float k = e.widening * clamp((e.keepEnd - float(leaf)) / max(e.tail, 1.0), 0.0, 1.0);
    ivec2 lf = forestLeaf(leaf);
    vec3 P, F, lc;
    mat3 R;
    float lr;
    bool onShoot;
    ForestLeafAt at;
    forestApplyClumpFlat(e.instance);
    if (k <= 0.0 || procClumpBare(e.instance, lf.x) || !forestLeafFrame(e.instance, lf, P, R, F, lc, lr, onShoot, at))
        return p;
    forestSprayBrush(e.instance, lf.x, lc, lr, P, R, F);
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);

    // A widened card stands in for several leaves. A spray's moves in
    // toward its lobe's centre by the length it grew, so it stays inside
    // the crown; a broad leaf stays on its node and grows from its petiole
    // (its twig is drawn: it must not leave it). Its light (depth in the
    // crown, occlusion, tint) stays the leaf's own, from where it hangs. A
    // cluster (mesh.frag's SCENE_FOREST_CLUSTER) stays where the kept leaf
    // hangs and is not scaled: the vertex sets out its hexagon by the
    // widening.
    bool cluster = (meshFlags() & MESH_FOREST_CLUSTER) != 0u;
    vec3 rest = P;
    float grown = (k - 1.0) * FOREST_HEADER(FOREST_H_LEAF_LENGTH);
    if (grown > 0.0 && !cluster && !onShoot) P = lc + (P - lc) * clamp(1.0 - 0.5 * grown / max(lr, 1e-3), 0.55, 1.0);
    // A spray's card grows by k^0.8, not k: the k^2 sprays it stands for
    // overlap where they hang together, so they cover less than k^2 of one.
    // (Grown by k, a pine's widened sprays covered 12.6% of the pane against
    // its leaves' 11.2% at 45 m, 0.7 to 0.9 dY darker; at k^0.8, 11.4%, 0.14
    // to 0.20.)
    if (!cluster) R *= forestLeafForm() != 0 ? pow(k, 0.8) : k;
    mat3 M3 = mat3(model) * R;
    vec3 center = (model * vec4(P, 1.0)).xyz;

    // The sway (forestLeafSway's, a leaf at a time): the wood's pose where
    // it hangs, then its own turn.
    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(lf.x) + FOREST_CLUMP_VEGETATION / 4, rest, 1.0, v0, v1, v2, v3);
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, anchor, clamp(v2.w, 0.0, 1.0));
    float amount = forestFlutterAt(e.widening, model, anchor, pose.drag);
    if (onShoot) {
        // Its spray's and shoot's bending (the twig it hangs from moves
        // the same, mesh_forest_tube.vert's twig build).
        vec3 off, om;
        forestLeafShootSway(at, model, pose.drag, amount, off, om);
        pose.offset += off;
        pose.omega += om;
    }
    float seed = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719)) + treeSeed * 61.7) * 43758.5453);
    vec3 side;
    float fold;
    mat3 rot = vegLeafTurn(pose, w, normalize(mat3(model) * F), normalize(mat3(model) * R[1]), seed, amount, side,
                           fold);

    p.alive = true;
    p.at = anchor + pose.offset + rot * (center - anchor);
    p.M = rot * M3;
    // The leaf's frame is a mirror image (clumpLeafFrame: across, normal,
    // tip left-handed), so turning the world's half on `side` by +fold
    // about the stem is turning the mesh's -x half by -fold: each half of
    // the mesh turns toward its normal, both rising.
    p.fold = vec2(cos(fold), sin(fold));
    p.shade = vegCrownShade(model, v1, anchor);
    p.tint = vegetationTint(model, v0, v1, v2, v3, true) * forestSpecimenTint(specimen, false);
    p.seed = fract(float(leaf) * 0.618034 + treeSeed);
    p.lobeOut = vec3(0.0, 1.0, 0.0);
    p.lobeSphere = vec4(0.0);
    p.lobeDensity = 0.0;
    p.lobeFlat = 1.0;
    if (!cluster) {
        // Each leaf its own lightness about the tree's (as a cluster's and a
        // lobe's leaves are, forestMassGrain): a sunlit side is leaves, not
        // one sheet. The mean is the tree's.
        p.tint *= 0.85 + 0.3 * fract(p.seed * 7.31 + 0.137);
    } else {
        vec3 outward = rest - lc;
        float upBias = FOREST_HEADER(FOREST_H_UP_BIAS);
        vec3 B = (dot(outward, outward) > 1e-10 ? normalize(outward) : vec3(0.0, 1.0, 0.0)) * (1.0 - upBias) +
                 vec3(0.0, upBias, 0.0);
        B = mat3(model) * B;
        p.lobeOut = dot(B, B) > 1e-10 ? normalize(rot * B) : vec3(0.0, 1.0, 0.0);
        // The lobe moves with its leaves (as a lobe sprite's sphere does),
        // flattened as clumpLeafFrame's: centred a tenth of its height up.
        float scale = vegScale(model);
        float r = max(lr * scale, 1e-3);
        p.lobeSphere = vec4((model * vec4(lc + vec3(0.0, 0.1 * lr * procClumpFlat, 0.0), 1.0)).xyz - center, r);
        // forestMassDepth's: its leaves (a third of the clump's), each
        // turned every way, over the disc, through the middle, a unit length.
        float leaves = max(FOREST_FLOAT(forestClumpRecord(lf.x), FOREST_CLUMP_LEAVES) / 3.0, 1.0);
        float a = 0.5 * FOREST_HEADER(FOREST_H_LEAF_LENGTH) * scale / r;
        p.lobeDensity = leaves * a * a * FOREST_HEADER(FOREST_H_LEAF_ASPECT) * 0.55 * 0.5 / r;
        p.lobeFlat = 0.8 * procClumpFlat;
    }
    return p;
}
#endif

// A leaf's caster (the drawer hands casters fewer, wider leaves: a shadow
// texel is wider than a leaf), placed and swayed as the leaf is, so its
// shadow on the leaves under it stays with it as a gust bends its spray, and
// folded about its stem as the leaf (each half its way, by the vertex). Its
// card is not narrowed here: the vertex narrows a spray's to the share its
// needles cover (forestSprayFill), across the card before its fold.
ForestCasterPose forestCasterPoseLeaf(ForestEntry e, int leaf) {
    ForestCasterPose p;
    p.alive = false;
    float k = e.widening * clamp((e.keepEnd - float(leaf)) / max(e.tail, 1.0), 0.0, 1.0);
    ivec2 lf = forestLeaf(leaf);
    vec3 P, F, lc;
    mat3 R;
    float lr;
    bool onShoot;
    ForestLeafAt at;
    forestApplyClumpFlat(e.instance);
    if (k <= 0.0 || procClumpBare(e.instance, lf.x) || !forestLeafFrame(e.instance, lf, P, R, F, lc, lr, onShoot, at))
        return p;
    forestSprayBrush(e.instance, lf.x, lc, lr, P, R, F);
    int specimen;
    float treeSeed;
    mat4 model = sceneModel() * forestInstance(e.instance, specimen, treeSeed);
    vec3 rest = P;
    float grown = (k - 1.0) * FOREST_HEADER(FOREST_H_LEAF_LENGTH);
    // A widened broad caster grows from its node as its leaf does
    // (forestPoseLeaf).
    if (grown > 0.0 && !onShoot) P = lc + (P - lc) * clamp(1.0 - 0.5 * grown / max(lr, 1e-3), 0.55, 1.0);
    mat3 M3 = mat3(model) * (R * k);
    vec3 center = (model * vec4(P, 1.0)).xyz;
    // A widened caster holds the area of the leaves it stands for, but
    // reaches further from its centre than they do: its shadow would fall on
    // leaves that stand above it toward the light. Set back away from the
    // light by part of the reach it gained, the crown's shadow starts about
    // where the leaves' does. At 0.35 of it, set back past leaves it should
    // shade: a broadleaf's thinned crown at golden 45 / 75 m 1.5 / 7.0
    // levels brighter than its leaves; at 0.15, 0.2 / 5.5 (at none, -0.6 /
    // 4.1).
    p.setback = grown > 0.0 ? 0.15 * grown * vegScale(model) : 0.0;

    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(lf.x) + FOREST_CLUMP_VEGETATION / 4, rest, 1.0, v0, v1, v2, v3);
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, anchor, clamp(v2.w, 0.0, 1.0));
    float amount = forestFlutterAt(e.widening, model, anchor, pose.drag);
    if (onShoot) {
        vec3 off, om;
        forestLeafShootSway(at, model, pose.drag, amount, off, om);
        pose.offset += off;
        pose.omega += om;
    }
    float seed = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719)) + treeSeed * 61.7) * 43758.5453);
    vec3 side;
    float fold;
    mat3 rot = vegLeafTurn(pose, w, normalize(mat3(model) * F), normalize(mat3(model) * R[1]), seed, amount, side,
                           fold);
    p.alive = true;
    p.at = anchor + pose.offset + rot * (center - anchor);
    p.M = rot * M3;
    p.fold = vec2(cos(fold), sin(fold));
    return p;
}

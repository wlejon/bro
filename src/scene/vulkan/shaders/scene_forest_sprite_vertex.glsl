// A forest sprite's centre, size, pose and look (mesh_forest_sprite.vert,
// shadow_forest_sprite.vert): see mesh_forest_sprite.vert for the modes.

struct ForestSprite {
    vec3 center;     // posed, camera-relative
    float radius;    // world: the sphere its leaves lie under (a crown's across)
    float radiusUp;  // world: up (a crown's height over two; a lobe's radius)
    vec2 corner;     // -1 .. 1
    float seed;
    float gust;      // the drag at the tree now (scene_vegetation.glsl's VegPose.drag)
    vec3 tint;       // the tree's and limb's leaf tint (scene_vegetation.glsl's vegLimbLeafTint)
    float fade;
    vec4 crown;      // the crown's centre (posed, camera-relative) and radius (world)
    float leaves;    // the leaves it holds
    float leafLen;   // a leaf's length (world)
    vec3 offset;     // the pose's offset from rest
    int instance;    // the tree
    // A core (mode 2): the lobe's radius (world), its height over its width,
    // its leaves' optical depth a unit length (world, forestLobeCore's) and
    // a leaf's length (world).
    vec4 core;
    // Its clumps' height over their width (0.8 times the specimen's
    // flatness): the ellipsoid the light leaves a lobe or clump through.
    float lightFlat;
};

bool forestSprite(int mode, int instanceIndex, int vertexIndex, mat4 base, out ForestSprite s) {
    s.center = vec3(0.0);
    s.radius = s.radiusUp = 0.0;
    s.corner = vec2(0.0);
    s.seed = s.gust = s.fade = 0.0;
    s.tint = vec3(1.0);
    s.crown = vec4(0.0, 0.0, 0.0, 1.0);
    s.leaves = 1.0;
    s.leafLen = 0.0;
    s.offset = vec3(0.0);
    s.instance = 0;
    s.lightFlat = 1.0;

    // corner 0..5: two triangles of the quad.
    int corner = vertexIndex % 6;
    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                     vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    s.corner = kCorners[corner];

    ForestEntry e;
    int clump = -1;
    bool core = mode == 2;
    if (mode == 0 || core) {
        e = forestEntry(instanceIndex);
        clump = e.first + vertexIndex / 18;
        if (clump >= e.end || procClumpBare(e.instance, clump)) return false;
    } else {
        e = forestEntry(vertexIndex / 6);
    }
    int specimen;
    float treeSeed;
    mat4 model = base * forestInstance(e.instance, specimen, treeSeed);
    float scale = vegScale(model);
    int sr = forestSpecimenRecord(specimen);
    s.leafLen = FOREST_HEADER(FOREST_H_LEAF_LENGTH) * scale;
    s.lightFlat = 0.8 * forestClumpFlat(specimen);

    vec3 c;           // specimen space
    float weight;
    vec4 v0, v1, v2, v3;
    s.core = vec4(0.0);
    if (mode == 0 || core) {
        int lobe = (vertexIndex / 6) % 3;
        vec4 rec = FOREST_RECORD(forestClumpRecord(clump), FOREST_CLUMP_CENTRE);
        float lr;
        procClumpFlat = forestClumpFlat(specimen);
        // The lobe's height over its width: a clump's leaf shell is 0.9 of
        // its radius up and 0.7 down (clumpLeafFrame), times the specimen's
        // flatness; a pinnate run's lobes are round (its leaflets line the
        // wood whichever way it runs: up and down along a hanging whip, where
        // the shell's squash left a fifth of the run between its lobes bare).
        bool pinnate = int(forestSpecimenShoot(specimen).y + 0.5) == 3;
        if (pinnate) {
            // A pinnate clump's leaves line its wood (forestPinnateLeaf): its
            // lobes a run of three along it, each a third of the run and a
            // leaflet's reach either side.
            vec3 D = FOREST_RECORD(forestClumpRecord(clump), FOREST_CLUMP_TWIG).xyz;
            D = dot(D, D) > 1e-12 ? normalize(D) : vec3(0.0, 1.0, 0.0);
            c = rec.xyz - D * (0.35 * rec.w) + D * (rec.w * (float(lobe) - 1.0) * 0.6667);
            lr = max(rec.w / 3.0, 0.8 * FOREST_HEADER(FOREST_H_LEAF_LENGTH));
            procClumpFlat = 1.0;
        } else {
            procClumpLobe(rec, clump, lobe, floatBitsToUint(FOREST_HEADER(FOREST_H_SEED)), c, lr);
            // The lobe's leaves fill an ellipsoid: flattened underneath, 0.9 of
            // the radius up and 0.7 down (clumpLeafFrame), times the
            // specimen's flatness.
            c.y += 0.1 * lr * procClumpFlat;
        }
        // Wider by the entry's reach (its `widening`, 1 or more): the fine
        // wood the bark's cut leaves out, whose cover the lobes take over.
        s.radius = lr * scale * (core ? 1.0 : max(e.widening, 1.0));
        float lobeUp = pinnate ? 1.0 : 0.8 * procClumpFlat;
        s.radiusUp = lobeUp * s.radius;
        // A lobe holds about a third of its clump's leaves (clumpLeafFrame).
        s.leaves = max(FOREST_FLOAT(forestClumpRecord(clump), FOREST_CLUMP_LEAVES) / 3.0, 1.0);
        forestVegetation(forestClumpRecord(clump) + FOREST_CLUMP_VEGETATION / 4, c, 1.0, v0, v1, v2, v3);
        weight = v2.w;
        s.seed = fract(float(clump * 3 + lobe) * 0.618034 + treeSeed);
        if (core) {
            // The lobe's CORE (forestLobeCore): its depth past which its own
            // leaves hide everything, drawn solid in their place.
            float sigma;
            float rc = forestLobeCore(lr, s.leaves, sigma);
            if (rc <= 0.0) return false;
            s.core = vec4(lr * scale, lobeUp, sigma / scale, s.leafLen);
            s.radius = rc * scale;
            s.radiusUp = lobeUp * s.radius;
            s.leafLen = 0.0;
        }
    } else {
        vec4 crown = FOREST_RECORD(sr, FOREST_SP_CROWN);
        vec4 more = FOREST_RECORD(sr, FOREST_SP_CROWN_UP);
        vec4 counts = FOREST_RECORD(sr, FOREST_SP_LEAF_START);
        c = crown.xyz;
        // The clumps' whole extent (ForestNode keeps 0.92 of it).
        s.radius = crown[FOREST_SP_CROWN_RADIUS % 4] / 0.92 * scale;
        s.radiusUp = more[FOREST_SP_CROWN_UP % 4] / 0.92 * scale;
        s.leaves = max(float(floatBitsToUint(counts[FOREST_SP_LEAF_COUNT % 4])), 1.0);
        // Posed as the clumps it stands in for: their mean branch weight,
        // on the limb of the clump at the crown's middle.
        forestVegetation(forestClumpRecord(int(floatBitsToUint(counts[FOREST_SP_CROWN_CLUMP % 4]))) + FOREST_CLUMP_VEGETATION / 4, c, 1.0, v0, v1, v2, v3);
        weight = counts.w;
        s.seed = treeSeed;
    }

    forestApplyLook();
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 rest = (model * vec4(c, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, rest, clamp(weight, 0.0, 1.0));
    s.center = rest + pose.offset;
    s.offset = pose.offset;
    s.instance = e.instance;
    s.gust = pose.drag;
    s.fade = e.fade;
    s.tint = vegLimbLeafTint(model, v0, v3) * forestSpecimenTint(specimen, false) * e.wood;
    // The crown's centre moves with the sprite, so a leaf's depth in it (its
    // light) is the rest pose's.
    s.crown = vec4((model * vec4(v1.xyz, 1.0)).xyz + pose.offset, max(v1.w * scale, 0.5));
    return true;
}

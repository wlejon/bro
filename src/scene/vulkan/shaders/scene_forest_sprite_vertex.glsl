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
    int corner = vertexIndex % FOREST_CARD_VERTICES;
    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                     vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    s.corner = kCorners[corner];

    ForestEntry e;
    int clump = -1;
    bool core = mode == FOREST_SPRITE_CORES;
    bool runs = mode == FOREST_SPRITE_RUNS;
    int run = -1;
    if (mode == FOREST_SPRITE_CLUMPS || core) {
        e = forestEntry(instanceIndex);
        clump = e.first + vertexIndex / FOREST_CLUMP_VERTICES;
        if (clump >= e.end || procClumpBare(e.instance, clump)) return false;
    } else if (runs) {
        e = forestEntry(instanceIndex);
        run = e.first + vertexIndex / FOREST_CARD_VERTICES;
        if (run >= e.end) return false;
    } else {
        e = forestEntry(vertexIndex / FOREST_CARD_VERTICES);
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
    if (mode == FOREST_SPRITE_CLUMPS || core) {
        int lobe = (vertexIndex / FOREST_CARD_VERTICES) % FOREST_CLUMP_LOBES;
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
            // As deep in its leaves up and down as across: a flat lobe's
            // shell is thinner over and under it, so the core is flatter
            // than its lobe (a pine's pads, its core its lobe's shape, showed
            // their cores' undersides from below as tan discs), and a lobe
            // too flat to hide one has none.
            float sigma;
            float rc = forestLobeCore(lr, s.leaves, sigma);
            float upR = lobeUp * lr - (lr - rc);
            if (rc <= 0.0 || upR <= 0.0) return false;
            s.core = vec4(lr * scale, upR / rc, sigma / scale, s.leafLen);
            s.radius = rc * scale;
            s.radiusUp = upR * scale;
            s.leafLen = 0.0;
        }
    } else if (runs) {
        // A RUN of the specimen's clumps (a caster): the upright ellipsoid
        // they fill, at the radii the crown trace takes it at (its form's
        // fill, FOREST_FILL_SHRINK), with the run's leaves; posed as its
        // first clump.
        int blk = floatBitsToInt(FOREST_FLOAT(sr, FOREST_SP_BLOCKS)) + run * FOREST_BLOCK_RECORDS;
        vec4 f = FOREST_RECORD(blk, FOREST_BLOCK_FILL);
        uint form = floatBitsToUint(FOREST_HEADER(FOREST_H_LOOK)) & 15u;
        float fill = (form == 1u ? 0.9 : form != 0u ? 1.0 : 0.8) * 0.9;
        c = f.xyz;
        s.radius = f.w * fill * scale;
        s.radiusUp = FOREST_FLOAT(blk, FOREST_BLOCK_FILL_UP) * fill * scale;
        s.leaves = max(FOREST_FLOAT(blk, FOREST_BLOCK_LEAVES), 1.0);
        uint first = floatBitsToUint(FOREST_RECORD(sr, FOREST_SP_SEG_START)[FOREST_SP_CLUMP_START % 4]);
        uint i0 = floatBitsToUint(FOREST_RECORD(blk, FOREST_BLOCK_CLUMPS).x) & 0xffffu;
        forestVegetation(forestClumpRecord(int(first + i0)) + FOREST_CLUMP_VEGETATION / 4, c, 1.0, v0, v1, v2, v3);
        weight = v2.w;
        s.seed = fract(float(run) * 0.618034 + treeSeed + 0.5);
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

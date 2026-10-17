// A forest sprite's centre, size, pose and look (mesh_forest_sprite.vert,
// shadow_forest_sprite.vert): see mesh_forest_sprite.vert for the modes.

struct ForestSprite {
    vec3 center;     // posed, camera-relative
    float radius;    // world: the sphere its leaves lie under (a crown's across)
    float radiusUp;  // world: up (a crown's height over two; a lobe's radius)
    vec2 corner;     // -1 .. 1
    float seed;
    float gust;      // the drag at the tree now (scene_vegetation.glsl's VegWind.p0)
    vec3 tint;       // the tree's and limb's leaf tint (scene_vegetation.glsl's vegLimbLeafTint)
    float fade;
    vec4 crown;      // the crown's centre (posed, camera-relative) and radius (world)
    float leaves;    // the leaves it holds
    float leafLen;   // a leaf's length (world)
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

    // corner 0..5: two triangles of the quad.
    int corner = vertexIndex % 6;
    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                     vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    s.corner = kCorners[corner];

    ForestEntry e;
    int clump = -1;
    if (mode == 0) {
        e = forestEntry(instanceIndex);
        clump = e.first + vertexIndex / 18;
        if (clump >= e.end) return false;
    } else {
        e = forestEntry(vertexIndex / 6);
    }
    int specimen;
    float treeSeed;
    mat4 model = base * forestInstance(e.instance, specimen, treeSeed);
    float scale = vegScale(model);
    int sr = forestSpecimenRecord(specimen);
    s.leafLen = procedural.header[1].z * scale;

    vec3 c;           // specimen space
    float weight;
    vec4 v0, v1, v2, v3;
    if (mode == 0) {
        int lobe = (vertexIndex / 6) % 3;
        vec4 rec = procedural.records[forestClumpRecord(clump)];
        float lr;
        procClumpLobe(rec, clump, lobe, floatBitsToUint(procedural.header[0].x), c, lr);
        s.radius = lr * scale;
        s.radiusUp = s.radius;
        // A lobe holds about a third of its clump's leaves (clumpLeafFrame).
        s.leaves = max(procedural.records[forestClumpRecord(clump) + 1].w / 3.0, 1.0);
        forestVegetation(forestClumpRecord(clump) + 2, c, 1.0, v0, v1, v2, v3);
        weight = v2.w;
        s.seed = fract(float(clump * 3 + lobe) * 0.618034 + treeSeed);
    } else {
        vec4 crown = procedural.records[sr + 2];
        vec4 more = procedural.records[sr + 3];
        vec4 counts = procedural.records[sr + 1];
        c = crown.xyz;
        // The clumps' whole extent (ForestNode keeps 0.92 of it).
        s.radius = crown.w / 0.92 * scale;
        s.radiusUp = more.x / 0.92 * scale;
        s.leaves = max(float(floatBitsToUint(counts.y)), 1.0);
        // Posed as the clumps it stands in for: their mean branch weight,
        // on the limb of the clump at the crown's middle.
        forestVegetation(forestClumpRecord(int(floatBitsToUint(counts.z))) + 2, c, 1.0, v0, v1, v2, v3);
        weight = counts.w;
        s.seed = treeSeed;
    }

    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 rest = (model * vec4(c, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, rest, clamp(weight, 0.0, 1.0));
    s.center = rest + pose.offset;
    s.gust = w.p0;
    s.fade = e.fade;
    s.tint = vegLimbLeafTint(model, v0, v3);
    // The crown's centre moves with the sprite, so a leaf's depth in it (its
    // light) is the rest pose's.
    s.crown = vec4((model * vec4(v1.xyz, 1.0)).xyz + pose.offset, max(v1.w * scale, 0.5));
    return true;
}

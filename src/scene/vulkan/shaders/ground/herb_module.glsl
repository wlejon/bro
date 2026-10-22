// The herb module of a grass node with none (scene_herb_layer.glsl): what
// the built-in cover shaders are compiled with. A world's module takes
// this file's place when its node's shaders are compiled at run time
// (scene_cover_shaders.h); the engine calls these only where
// grass.herbInfo.x says there is one.

int herbStand(ivec2 cell, vec2 xz, vec2 floorAt) { return -1; }

HerbMix herbFarMean(float canopy, float density, float tanView) {
    return HerbMix(0.0, vec3(0.0), vec3(0.0), 0.0);
}

float herbFarLocal(vec2 xz, float canopy, float u, float fp, out vec3 bloom, out vec3 pick) {
    bloom = vec3(0.0);
    pick = vec3(0.0);
    return 0.0;
}

HerbFarLook herbFarLook(int variety, vec4 h) {
    return HerbFarLook(0.0, 0.0, 0.0, vec3(0.0), vec3(0.0), 0.0);
}

#ifdef HERB_BUILD
bool herbBuild(HerbPlant p, int card, int cards, int verts, vec3 v, inout HerbVertex o) { return false; }
#endif

#ifdef HERB_SHADE
HerbShade herbShade(HerbFragment f) {
    return HerbShade(0.0, vec3(0.0), vec3(0.0, 1.0, 0.0), 1.0, 1.0, 1.0, 0.0, vec4(0.0));
}
#endif

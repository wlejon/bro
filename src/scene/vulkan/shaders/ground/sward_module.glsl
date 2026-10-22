// The sward module of a grass node with none (scene_sward_layer.glsl): what
// the built-in cover shaders are compiled with. A world's module takes this
// file's place when its node's shaders are compiled at run time
// (scene_cover_shaders.h). No blade stands and the turf is the bare ground.

void swardRoot(inout SwardCell s) {}

float swardDensity(inout SwardCell s) { return 0.0; }

SwardFarLook swardFarLook(SwardCell s, vec4 h) { return SwardFarLook(0.0, 0.0, vec3(0.0)); }

vec3 swardBake(vec2 xz, float fp) { return vec3(0.5); }

SwardTurf swardTurf(SwardTurfIn t) { return SwardTurf(t.soil, t.soil, 0.0); }

vec4 swardLay(vec2 xz, vec3 gn) { return vec4(gn, 0.0); }

#ifdef SWARD_BUILD
bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout SwardVertex o) { return false; }
#endif

#ifdef SWARD_SHADE
SwardShade swardShade(SwardFragment f) {
    return SwardShade(0.0, 0.0, 1e4, vec3(0.0), vec3(0.0, 1.0, 0.0), 1.0, 0.0, vec4(0.0));
}
#endif

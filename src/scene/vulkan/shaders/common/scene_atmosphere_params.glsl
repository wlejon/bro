// The atmosphere's block (std140), mirrored by AtmosphereGpu in
// scene/vulkan/frame/scene_vk_descriptors.h and filled by
// SceneAtmosphere::fill (scene/vulkan/lighting/scene_atmosphere.cpp) from
// AtmosphereParams (scene/world/atmosphere.h). It sits in the lighting
// block (lighting.atm) and is the atmosphere's compute shaders' uniform
// block. Lengths in metres; positions about the planet's centre ("air
// space"): `origin` is this block's eye there, so a camera-relative
// position p is at origin + p.

struct AtmosphereGpu {
    vec4 sunDir;          // xyz = unit vector towards the sun, w = 1 when the atmosphere is on
    vec4 sunIlluminance;  // rgb = the sun's illuminance at the top of the air, w = its angular radius
    vec4 rayleigh;        // rgb = scattering per metre at the ground, w = scale height
    vec4 mie;             // x = scattering, y = absorption per metre at the ground, z = g, w = scale height
    vec4 ozone;           // rgb = absorption per metre at the layer's peak, w = the peak's altitude
    vec4 shell;           // x = planet radius, y = top of the air's radius, z = ozone half width, w = spherical
    vec4 origin;          // xyz = the eye about the planet's centre, w = 0
    vec4 ground;          // rgb = ground albedo, w = the lights[] index of the sun (-1: none)
    vec4 skyView;         // x = r the sky-view table was made at, y = its mu_s, z = jitter on,
                          // w = the froxels' share of the air's lookups (sceneAtmosphereAir)
    vec4 froxel;          // xyz = this block's eye minus the froxel eye, w = the volume's depth (m)
    mat4 froxelViewProj;  // a position relative to the froxel eye -> its clip space
};

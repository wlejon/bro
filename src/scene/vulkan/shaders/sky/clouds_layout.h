// The cloud deck's shared constants (scene/world/clouds.h, vulkan/sky/
// scene_clouds.h): for C++ and GLSL alike.
//
//   The clouds' set (one layout for the march, the cover's bake and the
//   shadow's levels; what a shader does not read is bound to a stand-in):
//     CL_B_UBO        the clouds' block (CloudsGpu, sky/scene_cloud_layer.glsl)
//     CL_B_COVER      the cover, sampled: a cube of CL_COVER_SIDE texels a
//                     face (its border included), a face an array layer
//     CL_B_NOISE      the engine's tiling noise, sampled (CL_NOISE_SIDE^3,
//                     its mips down to a texel)
//     CL_B_DEPTH      the opaque depth (SceneTargets::depthSnapshot): the march
//     CL_B_COVER_OUT  the cover, written by the bake (rgba16f storage)
//     CL_B_SHADOW_OUT the shadow's levels, written (r32f storage, a level a layer)
//     CL_B_LAND_GRID  the land's processes' grid (the bake's cloudLandGrid)
//
//   The lighting set's binding 24: the shadow's levels, sampled
//   (CloudShadowGpu in the lighting block says where they lie).

#ifndef BRO_SCENE_CLOUDS_LAYOUT_H
#define BRO_SCENE_CLOUDS_LAYOUT_H

#define CL_B_UBO            0
#define CL_B_COVER          1
#define CL_B_NOISE          2
#define CL_B_DEPTH          3
#define CL_B_COVER_OUT      4
#define CL_B_SHADOW_OUT     5
#define CL_B_LAND_GRID      6

#define CL_MAX_DATA         64       // cloudData(i): vec4s

// The cover: a cube about the body, CL_COVER_SIDE texels a face, the first
// and last row and column a texel past the face's edge (the neighbouring
// face's ground), so a lookup bilinear across an edge is seamless.
#define CL_COVER_SIDE       1024
#define CL_COVER_GROUP      8

// The engine's noise: CL_NOISE_SIDE^3 texels, tiling, its mips to one texel.
#define CL_NOISE_SIDE       128
#define CL_NOISE_MIPS       8
#define CL_NOISE_GROUP      4

// The clouds' shadow on the land: CL_SHADOW_LEVELS square levels of
// CL_SHADOW_SIDE texels on the plane tangent to the body under the eye,
// level l's texel CL_SHADOW_TEXEL0 * CL_SHADOW_GROWTH^l metres (on that
// plane), each texel what the deck leaves of the sun at the datum there.
#define CL_SHADOW_LEVELS    7
#define CL_SHADOW_SIDE      256
#define CL_SHADOW_TEXEL0    60.0
#define CL_SHADOW_GROWTH    4.0
#define CL_SHADOW_STEPS     12
#define CL_SHADOW_GROUP     8

#endif

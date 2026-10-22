// The land's processes (vulkan/ground/scene_land_process.h): the runner's
// set and push constants, for C++ and GLSL alike. Every offset counts
// 32-bit words.
//
//   land_process.comp's set 0: binding 0 the grid read (rgba32f, an array
//            layer a root's channel: root * channels + channel), 1 the grid
//            written, 2 the halo's copies (uvec4: the destination texel and
//            the three it is the mean of, LandGrid::HaloCopy), 3 the
//            process's data (vec4s);
//            push constants LP_P_*

#ifndef BRO_LAND_PROCESS_LAYOUT_H
#define BRO_LAND_PROCESS_LAYOUT_H

#define LP_GROUP            8     // a workgroup is LP_GROUP x LP_GROUP cells of one root

#define LP_MODE_INIT        0     // procInit over every cell into the grid written
#define LP_MODE_PASS        1     // pass LP_P_PASS over every cell, read to written
#define LP_MODE_HALO        2     // the halo's copies within the grid written

#define LP_P_MODE           0     // uint: LP_MODE_*
#define LP_P_PASS           1     // uint: the pass (its index in the process's passes)
#define LP_P_ITERATION      2     // uint: the pass's iteration
#define LP_P_HALO           3     // uint: the halo's copies
#define LP_P_WORDS          4

#endif

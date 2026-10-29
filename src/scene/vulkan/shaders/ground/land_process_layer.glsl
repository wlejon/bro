// The land's processes' contract (docs/scene-world-architecture.md §11,
// "Processes"; scene/world/land_grid.h). A world's processes are GLSL that
// defines
//
//   void procInit(inout vec4 v[PROC_CHANNELS])
//       a cell's first values (v starts at zero, but v[0].y at 1);
//   void <pass>(inout vec4 v[PROC_CHANNELS])
//       each pass's kernel, named by the process's passes: the cell's next
//       values from its last (v starts as them) and its neighbours'
//       (procRead), run over every cell at once, the pass's iterations
//       times;
//
// and runs over the land's grid: PROC_CHANNELS vec4s a cell, a cell
// procCellSize() chart metres across, on a flat body one root over the
// land's extent, on a round one the six cube faces. Channel 0 is the
// land's height over its tree's: x added, y the tree's scale (its detail
// conditioned on the coarse state: none in the sea, more on a range), the
// height x + y * tree in the land's module and the CPU's heights alike; the
// rest is the world's, and the land's module reads every channel
// (landGrid(c, p)). It runs once, when the land
// is made (its module, extent, body or processes changed), and is read back
// for the CPU once (land_node.h): the land is drawn and its heights answer
// once that is in.
//
// A kernel reads, besides v:
//
//   vec4 procRead(int c, ivec2 o)     channel c of the cell o cells over (each
//                                     of o within 1: the halo holds a
//                                     neighbouring face's cells there)
//   vec4 procHere(int c)              procRead(c, ivec2(0)), as the pass began
//   vec3 procPoint()                  the cell's centre: a flat body's (x, 0,
//                                     z); a round body's datum point (body
//                                     frame, metres from its centre)
//   vec3 procPointAt(ivec2 o)         the centre of the cell o cells over
//   vec3 procUp()                     the body's up at the cell
//   vec2 procChart()                  its chart point (a face's metres)
//   int procRoot()                    its root (a round body's cube face)
//   float procCellSize()              a cell's side in chart metres (on a
//                                     round body a metre of ground is up to
//                                     1.4 of them near a face's corner:
//                                     procPointAt measures it)
//   int procIteration()               the pass's iteration, from 0
//   vec4 procData(int i)              the process's data (vec4 i of at most 64)
//   int procLevel(), int procBorder(), vec4 procCoarse(int c),
//   float procCoarseCellSize()        a regional grid's (ground/land_region_layer.glsl):
//                                     on the global grid -1, 2^20, procHere(c)
//                                     and procCellSize()
//   float landReliefRest(int L)       level L's share of its relief the last
//                                     procCoarse lacks (ground/land_regions.glsl;
//                                     1 on the global grid)
//
// and every other function a world writes; the land's tree is landTree(p)
// (its noise at the land's seed: 2D at a flat body's (x, z), 3D at a round
// body's datum point), and each regional level's relief landRelief(L, p)
// (processes.regions.relief; 0 past LAND_RELIEF_LEVELS of them).

#define PROC_CHANNELS LAND_GRID_CHANNELS

vec4 procRead(int c, ivec2 o) {
    ivec2 t = clamp(procCell_ + o, ivec2(0), ivec2(LAND_GRID_CELLS + 1));
    return imageLoad(procIn, ivec3(t, procRoot_ * LAND_GRID_CHANNELS + clamp(c, 0, LAND_GRID_CHANNELS - 1)));
}
vec4 procHere(int c) { return procRead(c, ivec2(0)); }

float procCellSize() { return landGridCell(); }
int procRoot() { return procRoot_; }
vec2 procChartAt(ivec2 o) { return -LAND_GRID_HALF + (vec2(procCell_ + o) - 0.5) * landGridCell(); }
vec2 procChart() { return procChartAt(ivec2(0)); }

vec3 procPointAt(ivec2 o) {
#ifdef LAND_GRID_ROUND
    return LAND_GRID_RADIUS * landGridDir(procRoot_, procChartAt(o));
#else
    vec2 x = procChartAt(o);
    return vec3(x.x, 0.0, x.y);
#endif
}
vec3 procPoint() { return procPointAt(ivec2(0)); }

vec3 procUp() {
#ifdef LAND_GRID_ROUND
    return landGridDir(procRoot_, procChart());
#else
    return vec3(0.0, 1.0, 0.0);
#endif
}

// (A regional grid's (ground/land_region_layer.glsl); on the global grid,
// level -1, no border, and no coarser field: its own.)
int procLevel() { return -1; }
int procBorder() { return 1 << 20; }
vec4 procCoarse(int c) { return procHere(c); }
float procCoarseCellSize() { return procCellSize(); }
float landReliefRest(int L) { return 1.0; }

int procIteration() { return int(pc.words[LP_P_ITERATION]); }
vec4 procData(int i) { return procDataBuffer.words[clamp(i, 0, 63)]; }

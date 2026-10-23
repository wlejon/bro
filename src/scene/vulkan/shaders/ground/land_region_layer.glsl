// The kernels' API on a regional grid (scene/world/land_regions.h): the same
// functions as on the global grid (ground/land_process_layer.glsl says what
// each is), over a region's cells, plus the field it starts from:
//
//   int procLevel()                   the region's level (0 the finest)
//   int procBorder()                  cells from the region's nearest edge (0 on it)
//   vec4 procCoarse(int c)            channel c of the next coarser field at the
//                                     cell (the coarser levels blended over the
//                                     global grid; a channel past the regions'
//                                     `keep` is the global grid's)
//   float procCoarseCellSize()        that field's cell, chart metres
//
// procRead's offsets past the region's edge read its edge cells.

#define PROC_CHANNELS LAND_GRID_CHANNELS

ivec2 procCell_;      // the cell (texel, from 0)
int procLevel_, procRoot_;
ivec2 procRegion_;    // the region's lattice indices

vec4 procRead(int c, ivec2 o) {
    ivec2 t = clamp(procCell_ + o, ivec2(0), ivec2(LAND_REGION_CELLS - 1));
    return imageLoad(procIn, ivec3(t, clamp(c, 0, LAND_GRID_CHANNELS - 1)));
}
vec4 procHere(int c) { return procRead(c, ivec2(0)); }

int procLevel() { return procLevel_; }
int procRoot() { return procRoot_; }
float procCellSize() { return landRegionCell(procLevel_); }
float procCoarseCellSize() {
    return procLevel_ + 1 < LAND_REGION_LEVELS ? landRegionCell(procLevel_ + 1) : landGridCell();
}
int procBorder() {
    ivec2 far = ivec2(LAND_REGION_CELLS - 1) - procCell_;
    return min(min(procCell_.x, procCell_.y), min(far.x, far.y));
}

vec2 procChartAt(ivec2 o) {
    float cell = landRegionCell(procLevel_);
    vec2 origin = -LAND_GRID_HALF + vec2(procRegion_) * landRegionStep(procLevel_) - float(LAND_REGION_APRON) * cell;
    return origin + (vec2(procCell_ + o) + 0.5) * cell;
}
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

vec4 procCoarse(int c) {
#ifdef LAND_GRID_ROUND
    return landFieldDirFrom(c, procUp(), procLevel_ + 1);
#else
    return landFieldAtFrom(c, procChart(), procLevel_ + 1);
#endif
}

int procIteration() { return int(pc.words[LR_P_ITERATION]); }
vec4 procData(int i) { return procDataBuffer.words[clamp(i, 0, 63)]; }

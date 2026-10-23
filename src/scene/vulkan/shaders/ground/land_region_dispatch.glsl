// What a region's run calls (vulkan/ground/scene_land_regions.cpp writes a
// land's: its regions' init and a switch over their passes). The built-in
// runner has neither.

void procRegionInit(inout vec4 v[PROC_CHANNELS]) {}
void procRegionRun(int pass, inout vec4 v[PROC_CHANNELS]) {}

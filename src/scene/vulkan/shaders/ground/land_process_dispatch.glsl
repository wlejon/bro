// Which kernel a pass runs (vulkan/ground/scene_land_process.cpp writes a
// land's: a switch over its passes' functions). The built-in runner has no
// passes.

void procRun(int pass, inout vec4 v[PROC_CHANNELS]) {}

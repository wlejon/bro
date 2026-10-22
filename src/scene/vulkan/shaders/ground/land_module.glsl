// The module of no land (scene_land_layer.glsl): what the built-in land fill
// is compiled with. A land node with no module of its own draws nothing.

float landHeight(vec2 p) { return 0.0; }

LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.18)); }

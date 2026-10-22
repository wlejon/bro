// The canopy of no plants (world/plant_canopy.h says what a canopy module
// defines): what a land's fill is compiled with when no plants stand on it.

vec2 plantsCanopy(vec2 p, out vec3 albedo) {
    albedo = vec3(0.0);
    return vec2(0.0);
}
vec2 plantsCanopy(vec3 p, out vec3 albedo) {
    albedo = vec3(0.0);
    return vec2(0.0);
}

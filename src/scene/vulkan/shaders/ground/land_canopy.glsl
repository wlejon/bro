// The canopy of no plants (world/plant_canopy.h says what a canopy module
// defines): what a land's fill is compiled with when no plants stand on it.

vec3 plantsCanopy(vec2 p, out vec3 albedo) {
    albedo = vec3(0.0);
    return vec3(0.0);
}
vec3 plantsCanopy(vec3 p, out vec3 albedo) {
    albedo = vec3(0.0);
    return vec3(0.0);
}

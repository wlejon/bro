// The built-in plant module (scene_plant_layer.glsl): no plants. The plants'
// built-in shaders compile with it; a plants node draws with its world's
// module in its place (PlantsNode::setModule), and without one draws nothing.

#ifdef PLANT_BUILD
bool plantElement(Plant p, PlantSpecies s, PlantCrown c, PlantElementIn k, inout PlantElement e) { return false; }
#endif

vec2 plantLeafPoint(int species, int c) { return vec2(0.0); }

bool plantLimb(Plant p, PlantSpecies s, PlantCrown c, int limb, out PlantLimb l) {
    l = PlantLimb(vec3(0.0), vec3(0.0), 0.0, 0.0, 0.0, vec3(0.0));
    return false;
}

vec3 plantSway(Plant p, PlantSpecies s, float up, float lag) { return vec3(0.0); }

#ifdef PLANT_SHADE
PlantSurface plantSurface(PlantFragment f) { return PlantSurface(f.albedo, normalize(f.normal), vec3(0.0)); }
#endif

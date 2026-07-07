// Depth-only instanced VS for the shadow caster pass. Same per-instance
// 4x3 affine layout as mesh_instanced.vert, but projects with absolute
// world positions (no camera-relative offset — shadows live in light space).

#version 330 core
layout(location = 0)  in vec3 aPos;
layout(location = 8)  in vec4 aInstRow0;
layout(location = 9)  in vec4 aInstRow1;
layout(location = 10) in vec4 aInstRow2;
uniform mat4 uLightVP;
uniform mat4 uModel;   // node's parent-chain world transform (e.g. TileWorld origin)
void main() {
    mat3 R = mat3(
        vec3(aInstRow0.x, aInstRow1.x, aInstRow2.x),
        vec3(aInstRow0.y, aInstRow1.y, aInstRow2.y),
        vec3(aInstRow0.z, aInstRow1.z, aInstRow2.z)
    );
    vec3 trans = vec3(aInstRow0.w, aInstRow1.w, aInstRow2.w);
    vec3 worldPos = (uModel * vec4(R * aPos + trans, 1.0)).xyz;
    gl_Position = uLightVP * vec4(worldPos, 1.0);
}

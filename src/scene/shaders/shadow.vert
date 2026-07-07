// Shadow caster shader — depth-only. Renders every shadow-casting MeshNode
// into one tile of the shadow atlas per shadow-casting light. The CPU
// pre-bakes uMVP = lightProj * lightView * meshWorldModel; no other material
// state matters because the FBO writes only depth (see shadow.frag).

#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}

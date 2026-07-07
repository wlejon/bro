// Skybox: render the IBL cubemap as the scene background. Reconstructs a
// world-space view direction from NDC + camera FOV/aspect (no matrix
// inverse needed — the view rotation transposed = view->world for the
// orthonormal basis). Drawn first into the HDR FBO with depth-test off
// so geometry simply paints over it.

#version 330 core
in vec3 vWorldDir;
out vec4 FragColor;
uniform samplerCube uEnv;
uniform float uIntensity;
uniform float uRotation;     // Y-axis rotation (radians), positive = clockwise looking down +Y
void main() {
    vec3 d = normalize(vWorldDir);
    float c = cos(uRotation), s = sin(uRotation);
    d = vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
    FragColor = vec4(texture(uEnv, d).rgb * uIntensity, 1.0);
}

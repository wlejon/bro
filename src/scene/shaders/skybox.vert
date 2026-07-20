
#version 330 core
layout(location = 0) in vec2 aPos;
out vec3 vWorldDir;
uniform mat3  uViewToWorld;
uniform float uTanHalfFovY;
uniform float uAspect;
void main() {
    vec3 viewDir = vec3(aPos.x * uTanHalfFovY * uAspect,
                        aPos.y * uTanHalfFovY,
                        -1.0);
    vWorldDir = uViewToWorld * viewDir;
    // z = 1 puts the quad at the far plane — even if depth test were on,
    // this would lose to anything with valid geometry depth.
    // Park the quad exactly on the far plane so it loses every depth test
    // against real geometry. Which clip-space z that is depends on the
    // convention: 1 conventionally, 0 under reversed-Z.
#ifdef REVERSED_Z
    gl_Position = vec4(aPos, 0.0, 1.0);
#else
    gl_Position = vec4(aPos, 1.0, 1.0);
#endif
}

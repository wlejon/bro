#version 450
// The depth drawn so far, each pixel's FARTHEST sample (PassFarDepth): what
// a forest sprite tests its card against before it places its leaves
// (mesh.frag's forestFarRejected). A fragment nearer than none of a pixel's
// samples fails the depth test at all of them, whatever it then writes, as a
// sprite writes no nearer than its card. FAR_DEPTH_MS: the MSAA depth.

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outDepth;

#ifdef FAR_DEPTH_MS
layout(set = 0, binding = 0) uniform sampler2DMS uDepth;
#else
layout(set = 0, binding = 0) uniform sampler2D uDepth;
#endif

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
#ifdef FAR_DEPTH_MS
    float d = texelFetch(uDepth, p, 0).r;
    for (int s = 1; s < textureSamples(uDepth); ++s) {
        float e = texelFetch(uDepth, p, s).r;
        d = min(d, e);   // (reversed-Z: the farthest is the least)
    }
    outDepth = d;
#else
    outDepth = texelFetch(uDepth, p, 0).r;
#endif
}

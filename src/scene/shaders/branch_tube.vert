// GPU procedural branch-tube vertex shader. Renders tapered tubes (the woody
// stems) with NO per-vertex geometry and NO per-instance buffer: the entire
// tube mesh is synthesised from gl_VertexID + a compact per-segment texture
// buffer. The app uploads ~a few thousand segments (2 texels each); one draw
// expands them into full tube walls in the vertex shader, so a growing
// skeleton re-uploads a few hundred KB instead of re-baking and re-uploading a
// multi-MB merged mesh every rebuild (the merged-mesh path costs a CPU
// interleave + a 6 MB glBufferData; this costs a small texture-buffer upload).
//
// Segment buffer uSegments (samplerBuffer RGBA32F), 2 texels per segment:
//   texel 2*i     = (from.xyz, radiusFrom)
//   texel 2*i + 1 = (to.xyz,   radiusTo)
//
// Topology: each segment is a uSides-sided open tube wall (no end caps — child
// segments overlap the joints, so caps would never show). Per segment =
// uSides quads = uSides*2 triangles = uSides*6 triangle-list vertices;
// gl_VertexID decodes to (segment, side, quad-corner).
//
// One source, two programs, selected by SHADOW_PASS:
//   forward : camera-relative clip via uVP/uCameraEye, full varyings + mesh frag
//   depth   : light-space clip via uLightVP, minimal, paired with shadow.frag

#version 330 core

uniform mat4 uInstModel;            // node parent-chain world transform
uniform samplerBuffer uSegments;   // packed per-segment records (2 texels each)
uniform int   uSides;              // tube sides (>= 3)
uniform float uRadiusScale;        // uniform radius multiplier

#ifdef SHADOW_PASS
uniform mat4 uLightVP;             // world-space light proj*view
#else
uniform mat4 uVP;                  // projection*view (camera-relative)
uniform vec3 uCameraEye;           // world-space eye, subtracted from tube origin

out vec3  vWorldPos;
out vec3  vNormal;
out vec2  vUV;
out vec4  vColor;
out float vCamDist;
out vec3  vTangentW;
out vec3  vBitangentW;
out vec4  vInstColor;
#endif

const float TWO_PI = 6.28318530718;

// Any unit vector perpendicular to unit `t` — matches perpendicularUnit() in
// the leaf scatter path so tube rings and leaf frames share a basis.
vec3 perpendicularUnit(vec3 t) {
    vec3 c = cross(t, vec3(0.0, 1.0, 0.0));
    if (dot(c, c) < 1e-8) c = cross(t, vec3(1.0, 0.0, 0.0));
    return normalize(c);
}

void main() {
    int perSeg = uSides * 6;
    int seg    = gl_VertexID / perSeg;
    int local  = gl_VertexID - seg * perSeg;
    int side   = local / 6;
    int corner = local - side * 6;

    // Side quad = two triangles across rings (b = bottom/from, t = top/to):
    //   corner 0=b0 1=b1 2=t0   3=t0 4=b1 5=t1
    bool top   = (corner == 2 || corner == 3 || corner == 5);
    bool useA1 = (corner == 1 || corner == 4 || corner == 5);
    float a0 = float(side)     / float(uSides) * TWO_PI;
    float a1 = float(side + 1) / float(uSides) * TWO_PI;
    float ang  = useA1 ? a1 : a0;
    float endT = top ? 1.0 : 0.0;

    vec4 s0 = texelFetch(uSegments, seg * 2);
    vec4 s1 = texelFetch(uSegments, seg * 2 + 1);
    vec3 from = s0.xyz; float rFrom = s0.w;
    vec3 to   = s1.xyz; float rTo   = s1.w;

    vec3 axis = to - from;
    float len = length(axis);
    if (len < 1e-6) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    vec3 T  = axis / len;
    vec3 e1 = perpendicularUnit(T);
    vec3 e2 = cross(T, e1);

    vec3  end    = mix(from, to, endT);
    float r      = mix(rFrom, rTo, endT) * uRadiusScale;
    vec3  radial = cos(ang) * e1 + sin(ang) * e2;
    vec3  pos    = end + r * radial;

    vec3 worldPos = (uInstModel * vec4(pos, 1.0)).xyz;

#ifdef SHADOW_PASS
    gl_Position = uLightVP * vec4(worldPos, 1.0);
#else
    vec3 camRel = worldPos - uCameraEye;
    mat3 nm = mat3(uInstModel);
    vNormal     = normalize(nm * radial);
    vTangentW   = normalize(nm * T);
    vBitangentW = cross(vNormal, vTangentW);
    vWorldPos   = camRel;
    vUV         = vec2(ang / TWO_PI, endT);
    vColor      = vec4(1.0);
    vInstColor  = vec4(1.0);
    vCamDist    = length(camRel);
    gl_Position = uVP * vec4(camRel, 1.0);
#endif
}

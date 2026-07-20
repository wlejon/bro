#version 330 core
// Screen-space reflections (opaque surfaces only, forward renderer style).
//
// Runs as a full-screen pass right after the opaque + decal passes and
// BEFORE every blended pass (translucents, splats, particles, billboards),
// so the inputs are coherent: uColorTex is a snapshot of the lit opaque HDR
// result whose ALPHA channel carries the per-pixel reflectance mask the mesh
// shaders wrote while the SSR mask phase was active (luminance of F0 scaled
// by perceptual smoothness^2 — see mesh.frag), and uDepthTex is the resolved
// opaque depth snapshot.
//
// Per fragment: reconstruct the view-space position from depth + the inverse
// projection (same recipe as ssao.frag — exact for perspective AND ortho
// cameras, no linearization shortcuts), face normal from position
// derivatives, reflect the incident ray and march the depth buffer in
// uSteps linear view-space steps. A front-to-behind depth crossing is
// binary-refined (6 halvings), then accepted only if the refined ray point
// sits within uThickness view units behind the surface — crossings deeper
// than that are silhouette fly-bys behind an occluder and the march
// continues, letting rays re-emerge. On a hit the HDR color at the hit
// pixel is blended over the surface, weighted by mask * uIntensity, faded
// at screen borders (uEdgeFade) and for rays reflecting back toward the
// camera (whose hit data is unreliable in screen space).
//
// The pass draws with blending DISABLED and rewrites every opaque pixel:
// rgb = mix(base, reflection, weight), alpha = 1.0 — which simultaneously
// restores the alpha channel from "reflectance mask" back to the coverage
// value the compositor reads (opaque geometry == fully covered). Sky /
// background pixels (depth >= 1) discard, leaving their color AND coverage
// (0 empty, 1 skybox) untouched.
//
// A miss changes nothing: IBL specular already provides the environment
// reflection, so SSR composites local geometry ON TOP of that fallback
// rather than replacing it.
//
// Derivative-dependent values are computed before the discard so the 2x2
// quad neighborhoods stay coherent (same rule as decal.frag).

in vec2 vUV;

uniform sampler2D uColorTex;   // opaque+decal HDR snapshot; a = reflectance mask
uniform sampler2D uDepthTex;   // resolved opaque depth snapshot
uniform mat4  uProj;           // camera projection
uniform mat4  uInvProj;        // inverse projection
uniform int   uPerspective;    // 1 = perspective, 0 = orthographic
uniform float uMaxDistance;    // max ray length, view/world units
uniform int   uSteps;          // linear march steps over uMaxDistance
uniform float uThickness;      // depth acceptance behind a surface, view units
uniform float uIntensity;      // reflection weight scale
uniform float uEdgeFade;       // screen-border fade width, fraction of viewport

out vec4 FragColor;

// Window-space UV + depth -> view-space position.
vec3 viewPos(vec2 uv, float d) {
    // Window depth is already the clip-space z under [0,1] clip control.
#ifdef REVERSED_Z
    vec4 clip = vec4(uv * 2.0 - 1.0, d, 1.0);
#else
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
#endif
    vec4 v = uInvProj * clip;
    return v.xyz / v.w;
}

void main() {
    vec4 src = texture(uColorTex, vUV);
    float d0 = texture(uDepthTex, vUV).r;
    vec3 P = viewPos(vUV, d0);
    // Face normal from position derivatives (before any discard).
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));

#ifdef REVERSED_Z
    if (d0 <= 0.0) discard;    // sky / empty: color + coverage stay untouched
#else
    if (d0 >= 1.0) discard;    // sky / empty: color + coverage stay untouched
#endif

    float mask = src.a;
    vec3 refl = vec3(0.0);
    float w = 0.0;

    if (mask * uIntensity > 0.002) {
        // Incident direction (eye -> surface). Ortho rays all travel -Z.
        vec3 I = (uPerspective == 1) ? normalize(P) : vec3(0.0, 0.0, -1.0);
        vec3 R = reflect(I, N);

        // Rays bending back toward the camera mostly hit surfaces whose far
        // side the depth buffer doesn't describe — fade them out.
        float facingFade = 1.0 - smoothstep(0.35, 0.9, dot(R, -I));

        if (facingFade > 0.001) {
            float stepLen = uMaxDistance / float(uSteps);
            // Nudge the origin off the surface so grazing rays don't
            // immediately self-intersect their own pixel neighborhood.
            vec3 O = P + N * min(0.05, stepLen * 0.5);

            float tPrev = 0.0;
            float dzPrev = -1.0;   // "in front of the depth buffer"
            float t = stepLen;
            for (int i = 0; i < uSteps; ++i) {
                vec3 Q = O + R * t;
                vec4 clip = uProj * vec4(Q, 1.0);
                if (uPerspective == 1 && clip.w <= 0.0) break;  // behind camera
                vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
                if (any(lessThan(uv, vec2(0.0))) ||
                    any(greaterThan(uv, vec2(1.0)))) break;     // left the screen

                float sd = texture(uDepthTex, uv).r;
                // dz > 0: ray point is behind the surface at this pixel.
#ifdef REVERSED_Z
                float dz = (sd > 0.0) ? viewPos(uv, sd).z - Q.z : -1.0;
#else
                float dz = (sd < 1.0) ? viewPos(uv, sd).z - Q.z : -1.0;
#endif

                if (dz > 0.0 && dzPrev <= 0.0) {
                    // Crossed the depth shell: bisect to the crossing point,
                    // then thickness-test the refined penetration.
                    float lo = tPrev, hi = t;
                    vec2 hitUV = uv;
                    float hitDz = dz;
                    for (int j = 0; j < 6; ++j) {
                        float mid = 0.5 * (lo + hi);
                        vec3 Qm = O + R * mid;
                        vec4 cm = uProj * vec4(Qm, 1.0);
                        vec2 um = (cm.xy / cm.w) * 0.5 + 0.5;
                        float sm = texture(uDepthTex, um).r;
#ifdef REVERSED_Z
                        float dm = (sm > 0.0) ? viewPos(um, sm).z - Qm.z : -1.0;
#else
                        float dm = (sm < 1.0) ? viewPos(um, sm).z - Qm.z : -1.0;
#endif
                        if (dm > 0.0) { hi = mid; hitUV = um; hitDz = dm; }
                        else          { lo = mid; }
                    }
                    if (hitDz <= uThickness) {
                        vec2 b = min(hitUV, 1.0 - hitUV);
                        float edge = (uEdgeFade > 0.0)
                            ? smoothstep(0.0, uEdgeFade, min(b.x, b.y))
                            : 1.0;
                        refl = texture(uColorTex, hitUV).rgb;
                        w = clamp(mask * uIntensity, 0.0, 1.0)
                          * edge * facingFade;
                        break;
                    }
                    // Deeper than the tolerance: a silhouette fly-by behind
                    // an occluder — keep marching so the ray can re-emerge.
                }
                dzPrev = dz;
                tPrev = t;
                t += stepLen;
            }
        }
    }

    // Composite + coverage restore in one write (see header comment).
    FragColor = vec4(mix(src.rgb, refl, w), 1.0);
}

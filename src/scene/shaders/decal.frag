#version 330 core
// Screen-space projected decal (Godot Decal analog, forward-renderer style).
//
// Per fragment: reconstruct the opaque scene position from the scene depth
// snapshot (works for perspective AND ortho — the full inverse view-
// projection is applied, no linearization shortcuts), transform it into the
// decal's local space, discard outside the unit box, then blend
// albedo * modulate onto the LIT HDR result. Because there is no G-buffer,
// the decal cannot modify material inputs pre-lighting like Godot does;
// instead the albedo is multiplied by a cheap lighting approximation
// (uAmbient + dominant-directional NdotL/PI — the same magnitudes the mesh
// shader's flat-ambient + Lambert diffuse produce) so decals darken with
// scene lighting instead of glowing in the dark. No shadows, no IBL, no
// point/spot contribution — documented in scene-api.js.
//
// The surface normal is reconstructed from screen-space derivatives of the
// reconstructed position: exact on planar surfaces, faceted on curved ones,
// noisy at depth discontinuities. It drives the normalFade cutoff and the
// directional lighting term. Derivative-dependent values are computed BEFORE
// any discard so neighboring helper invocations stay coherent.
//
// uSceneDepth is a snapshot (glBlitFramebuffer) of the opaque depth — the
// draw FBO's own depth attachment can't be sampled in strict GL 3.3 (feedback
// loop), same rule as the soft-particle pass.
//
// Output is premultiplied; the pass blends (ONE, ONE_MINUS_SRC_ALPHA) for
// color and (ZERO, ONE) for alpha so decals never alter the coverage the
// compositor reads (they only appear on already-opaque pixels anyway).

uniform sampler2D uSceneDepth;   // opaque scene depth snapshot
uniform sampler2D uAlbedoTex;    // RGBA8 albedo (straight alpha)
uniform sampler2D uEmissionTex;  // RGB emission
uniform int uHasAlbedo;          // 0 = plain modulate color, no texture
uniform int uHasEmission;

uniform vec2 uViewport;          // render-target size in pixels
uniform mat4 uInvViewProj;       // clip -> camera-relative world
uniform mat4 uInvModel;          // camera-relative world -> decal local
uniform vec3 uDecalUp;           // decal local +Y axis in world space (unit)

uniform vec4 uModulate;          // albedo tint (rgb) x master opacity (a)
uniform float uEmissionStrength;
uniform float uUpperFade;        // falloff exponent toward local +Y end; 0 = off
uniform float uLowerFade;        // falloff exponent toward local -Y end; 0 = off
uniform float uNormalFade;       // surface-facing cutoff [0,1); 0 = off

uniform vec3 uAmbient;           // scene flat ambient
uniform vec3 uSunDir;            // dominant directional light dir (light -> scene)
uniform vec3 uSunColor;          // its color * intensity; 0 when none

out vec4 FragColor;

const float PI = 3.14159265359;

void main() {
    vec2 screenUV = gl_FragCoord.xy / uViewport;
    float d = texture(uSceneDepth, screenUV).r;

    // Reconstruct the camera-relative world position of the opaque surface.
    vec4 ndc = vec4(screenUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 pw = uInvViewProj * ndc;
    vec3 camRel = pw.xyz / pw.w;

    // Screen-space surface normal — must be computed before any discard so
    // the derivatives read coherent neighbor values. cross(ddx, ddy) points
    // toward the viewer for front-facing surfaces (GL window space is y-up),
    // i.e. out of the surface.
    vec3 nrm = normalize(cross(dFdx(camRel), dFdy(camRel)));

    if (d >= 1.0) discard;                 // sky / no opaque geometry

    vec3 local = (uInvModel * vec4(camRel, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;   // outside the box

    // Projection UV: looking down the -Y projection axis, U maps local +X
    // and V maps local +Z.
    vec2 uv = local.xz + 0.5;

    // Fades along the projection axis: full strength at the volume's center
    // plane, pow-shaped falloff toward each end.
    float fade = 1.0;
    if (uUpperFade > 0.0 && local.y > 0.0)
        fade *= pow(clamp(1.0 - local.y * 2.0, 0.0, 1.0), uUpperFade);
    if (uLowerFade > 0.0 && local.y < 0.0)
        fade *= pow(clamp(1.0 + local.y * 2.0, 0.0, 1.0), uLowerFade);

    // Normal fade: cut surfaces facing away from the projection direction
    // (Godot's curve: smoothstep over the remapped cosine).
    float facing = dot(nrm, uDecalUp);
    if (uNormalFade > 0.0)
        fade *= smoothstep(uNormalFade, 1.0, facing * 0.5 + 0.5);

    fade *= uModulate.a;
    if (fade <= 0.001) discard;

    vec4 albedo = (uHasAlbedo == 1) ? texture(uAlbedoTex, uv)
                                    : vec4(1.0);
    float a = albedo.a * fade;

    // Cheap lit approximation: flat ambient + Lambert diffuse from the
    // dominant directional light, magnitudes matching mesh.frag's
    // uAmbient * baseColor and kD * baseColor / PI * radiance * NdotL terms.
    vec3 lit = uAmbient + uSunColor * (max(dot(nrm, -uSunDir), 0.0) / PI);
    vec3 rgb = albedo.rgb * uModulate.rgb * lit * a;

    if (uHasEmission == 1) {
        // Emission ignores scene lighting (self-lit) and the albedo's alpha,
        // but respects the volume fades + master opacity so it never leaks
        // past the decal's spatial envelope.
        rgb += texture(uEmissionTex, uv).rgb * uModulate.rgb
               * uEmissionStrength * fade;
    }

    FragColor = vec4(rgb, a);
}

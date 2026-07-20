#version 330 core

// Analytic sky. Shares skybox.vert (a fullscreen triangle pair that hands us a
// world-space view direction) and swaps the cubemap lookup for a real
// integration through the atmosphere — see atmosphere.glsl, which the renderer
// splices in directly below the directive above.
//
// That directive stays on line 1, and the word must not appear anywhere above
// it — insertAfterVersion takes the first literal hit, so a mention inside a
// leading comment would splice the whole atmosphere into the comment block.
//
// Drawn with depth test off before the geometry passes, exactly like the
// cubemap skybox it replaces.

in vec3 vWorldDir;
out vec4 FragColor;

uniform vec3  uCamPos;
uniform float uSunAngularRadius;   // radians; the real sun is about 0.00465
uniform float uSunDiskIntensity;   // scales the disk against the sky

// Marching steps. The sky is one fullscreen pass over background pixels only,
// so it can afford more than the aerial-perspective path does.
const int SKY_STEPS     = 24;
const int SKY_SUN_STEPS = 6;

void main() {
    vec3 rd = normalize(vWorldDir);

    vec3 col = atmSky(uCamPos, rd, SKY_STEPS, SKY_SUN_STEPS);

    // The sun itself, attenuated by the air between it and the viewer, which is
    // what turns it orange at the horizon without any special case for sunset.
    float mu = dot(rd, uAtmSunDir);
    if (mu > cos(uSunAngularRadius)) {
        vec3 ro = vec3(uCamPos.x,
                       uCamPos.y - uAtmSeaLevel + uAtmPlanetRadius,
                       uCamPos.z);
        if (atmHitDistance(ro, rd, uAtmPlanetRadius) < 0.0) {
            float t = atmExitDistance(ro, rd, uAtmPlanetRadius + uAtmThickness);
            if (t > 0.0) {
                col += uAtmSunColor * uSunDiskIntensity
                     * atmExtinction(atmOpticalDepth(ro, rd, t, SKY_SUN_STEPS));
            }
        }
    }

    FragColor = vec4(col, 1.0);
}

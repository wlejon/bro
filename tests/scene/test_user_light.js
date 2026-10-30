// A custom chunk's userLight (docs/scene-nodes-api.js, setShader): the
// occlusion a surface knows beyond the shadow maps.
//   - sunVisibility scales the sun's light;
//   - what hides the sky sends back the sun on it: with no sky seen and
//     occluderSun = 1, a surface of base colour rho under a sun of E
//     straight overhead reads rho * rho * E / pi (their radiance, of the
//     surface's albedo, over the whole hidden hemisphere), and nothing with
//     occluderSun = 0.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 40, near: 0.1, far: 100, position: [0, 4, 0.01], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0, 0, 0] });
        // pi lux straight down: a level white surface reflects 1.
        sc.createLight({ type: 'directional', direction: [0, -1, 0], color: [1, 1, 1], intensity: Math.PI });
    }
    return sc;
}

const S = 32;
const scene = freshScene(S);
if (!scene) {
    missingGpuContext('scene');
} else {
    const plane = scene.createMesh({
        positions: new Float32Array([-2, 0, -2, 2, 0, -2, -2, 0, 2, 2, 0, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        indices: new Uint32Array([0, 2, 1, 1, 2, 3]),
        color: [1, 1, 1],
    });
    plane.setShader({
        fragment: `uniform float u_sun;
            uniform float u_sky;
            uniform float u_occ;
            void userFragment(inout vec3 baseColor, inout vec3 normal, inout float metallic,
                              inout float roughness, inout vec3 emissive, inout float alpha) {
                baseColor = vec3(0.5);
                metallic = 0.0;
                roughness = 1.0;
            }
            void userLight(vec3 toSun, inout float sunVisibility, inout float skyVisibility,
                           inout vec3 skyNormal, inout float occluderSun) {
                sunVisibility = u_sun;
                skyVisibility = u_sky;
                occluderSun = u_occ;
            }`,
        uniforms: { u_sun: 1, u_sky: 1, u_occ: 0.5 },
    });
    const centre = () => {
        const img = scene.captureFrame();
        const i = ((S / 2) * img.width + S / 2) * 4;
        return img.data[i];
    };
    // Lambert, rho E mu / pi = 0.5, plus the specular's few percent.
    const lit = centre();
    assert(lit > 120 && lit < 150, `in the sun a level surface of 0.5 reads about 0.5 (${lit})`);
    plane.setShaderUniform('u_sun', 0.0);
    plane.setShaderUniform('u_sky', 0.0);
    plane.setShaderUniform('u_occ', 0.0);
    const dark = centre();
    assert(dark <= 2, `no sun, no sky, unlit occluders: black (${dark})`);
    plane.setShaderUniform('u_occ', 1.0);
    const back = centre();
    const want = 255 * 0.5 * 0.5;   // rho * rho * E / pi, E = pi
    assert(Math.abs(back - want) <= 4, `the sunlit occluders' light: rho^2 E / pi (${back} vs ${want.toFixed(1)})`);
    plane.setShaderUniform('u_occ', 0.5);
    const half = centre();
    assert(Math.abs(half - want / 2) <= 3, `half of them in sun, half the light (${half} vs ${(want / 2).toFixed(1)})`);
}

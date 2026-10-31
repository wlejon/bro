// A custom module's light hook (docs/scene-nodes-api.js, setShaderModule): the
// occlusion a surface knows beyond the shadow maps.
//   - sun_visibility scales the sun's light;
//   - what hides the sky sends back the sun on it: with no sky seen and
//     occluder_sun = 1, a surface of base colour rho under a sun of E
//     straight overhead reads rho * rho * E / pi (their radiance, of the
//     surface's albedo, over the whole hidden hemisphere), and nothing with
//     occluder_sun = 0;
//   - sun_diffuse comes down evenly from the sky above, as much as the sky
//     seen lets through.

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
        module: `
            module hook;
            import mesh;
            struct Params {
                sun: f32,
                sky: f32,
                occ: f32,
                dif: f32,
            }
            @binding(4, 0) var params: uniform<Params>;
            pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                var out = s;
                out.base_color = vec3<f32>(0.5);
                out.metallic = 0.0;
                out.roughness = 1.0;
                return out;
            }
            pub fn light(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                var out = s;
                out.sun_visibility = params.sun;
                out.sky_visibility = params.sky;
                out.occluder_sun = params.occ;
                out.sun_diffuse = params.dif;
                return out;
            }`,
        uniforms: { sun: 1, sky: 1, occ: 0.5, dif: 0 },
    });
    const centre = () => {
        const img = scene.captureFrame();
        const i = ((S / 2) * img.width + S / 2) * 4;
        return img.data[i];
    };
    // Lambert, rho E mu / pi = 0.5, plus the specular's few percent.
    const lit = centre();
    assert(lit > 120 && lit < 150, `in the sun a level surface of 0.5 reads about 0.5 (${lit})`);
    plane.setShaderUniform('sun', 0.0);
    plane.setShaderUniform('sky', 0.0);
    plane.setShaderUniform('occ', 0.0);
    const dark = centre();
    assert(dark <= 2, `no sun, no sky, unlit occluders: black (${dark})`);
    plane.setShaderUniform('occ', 1.0);
    const back = centre();
    const want = 255 * 0.5 * 0.5;   // rho * rho * E / pi, E = pi
    assert(Math.abs(back - want) <= 4, `the sunlit occluders' light: rho^2 E / pi (${back} vs ${want.toFixed(1)})`);
    plane.setShaderUniform('occ', 0.5);
    const half = centre();
    assert(Math.abs(half - want / 2) <= 3, `half of them in sun, half the light (${half} vs ${(want / 2).toFixed(1)})`);
    // The sun scattered down from the sky above: all of it, seen through the
    // whole sky, lights a level surface as the sun did (rho E mu / pi, no
    // specular); hidden with the sky, none.
    plane.setShaderUniform('occ', 0.0);
    plane.setShaderUniform('sky', 1.0);
    plane.setShaderUniform('dif', 1.0);
    const glow = centre();
    assert(Math.abs(glow - 127.5) <= 4, `the sun scattered down evenly lights it as the sun did (${glow})`);
    plane.setShaderUniform('sky', 0.0);
    const hidden = centre();
    assert(hidden <= 4, `with the sky hidden, the scattered sun is too (${hidden})`);
}

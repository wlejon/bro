// scene.createVolume: a medium over the whole view, its module's volume
// marching each pixel's ray up to the opaque depth, composited as
// scene * transmittance + its light.
//   - the composite: an emissive plane and the black sky behind a medium of
//     transmittance 0.5 and light 0.2 read plane * 0.5 + 0.2 and 0.2;
//   - ray.scene_dist is the opaque surface's distance along the ray (the camera
//     to the plane's centre at the view's centre), and huge over the sky;
//   - ray.dir is the unit ray (the view's centre looks along -camera);
//   - setShaderUniform reaches the module; a volume casts no shadow and does
//     not hide what it covers from the depth;
//   - the near grid: a medium given point by point (medium, medium_range) is
//     read to the opaque depth or its range, the march on from ray.near seen
//     through it, and a medium in one part of the four is cover;
//   - a module without volume, or a medium without medium_range, is refused;
//   - temporal reconstruction averages per-render noise;
//   - the incident exposure meter reads the volume's rays at the scene's
//     footprint scale.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 50, near: 0.1, far: 100, position: [0, 2.2, 3.2], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0, 0, 0] });
        sc.createLight({ type: 'directional', intensity: 0 });
    }
    return { canvas: cv, scene: sc };
}

// A 4 x 4 plane at y = 0 glowing `rgb`.
function plane(scene, rgb) {
    const node = scene.createMesh({
        positions: new Float32Array([-2, 0, -2, 2, 0, -2, -2, 0, 2, 2, 0, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        indices: new Uint32Array([0, 2, 1, 1, 2, 3]),
        color: [0, 0, 0],
    });
    node.setShader({
        module: `
            module hook;
            import mesh;
            struct Params { glow: vec3<f32> }
            @binding(4, 0) var params: uniform<Params>;
            pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                var out = s;
                out.base_color = vec3<f32>(0.0);
                out.metallic = 0.0;
                out.roughness = 1.0;
                out.emissive = params.glow;
                return out;
            }`,
        uniforms: { glow: rgb },
    });
    return node;
}

const px = (img, x, y) => {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
};
const near = (a, b, tol) => a.every((v, i) => Math.abs(v - b[i]) <= tol);

const S = 64;
const probe = freshScene(S);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    const { scene } = probe;
    let threw = null;
    plane(scene, [0.6, 0.2, 0.1]);
    const before = scene.captureFrame();
    const centre = px(before, S / 2, S / 2), sky = px(before, S / 2, 2);
    assert(near(centre, [153, 51, 26], 3) && near(sky, [0, 0, 0], 1),
           `the plane glows and the sky is black (${centre}, ${sky})`);

    const fog = scene.createVolume({
        module: `
            module hook;
            import volume;
            struct Params {
                t: f32,
                light: vec3<f32>,
            }
            @binding(4, 0) var params: uniform<Params>;
            pub fn volume(ray: volume::Ray) -> volume::Light {
                return volume::light(params.light, params.t, ray.near);
            }`,
        uniforms: { t: 0.5, light: [0.2, 0.2, 0.2] },
    });
    assert(fog && fog.hasShader, 'createVolume makes a node with its module');
    let img = scene.captureFrame();
    assert(near(px(img, S / 2, S / 2), [127, 76, 64], 3),
           `over the plane: plane * 0.5 + 0.2 (${px(img, S / 2, S / 2)})`);
    assert(near(px(img, S / 2, 2), [51, 51, 51], 2), `over the sky: 0.2 (${px(img, S / 2, 2)})`);

    fog.setShaderUniform('t', 1.0);
    fog.setShaderUniform('light', [0, 0, 0]);
    img = scene.captureFrame();
    assert(near(px(img, S / 2, S / 2), centre, 2) && near(px(img, S / 2, 2), sky, 1),
           'a clear medium leaves the scene as it was (setShaderUniform reaches the module)');
    scene.destroyNode(fog);

    // The distance to the opaque surface and the ray, as colours.
    const probeVol = scene.createVolume({
        module: `
            module hook;
            import volume;
            pub fn volume(ray: volume::Ray) -> volume::Light {
                return volume::light(vec3<f32>(min(ray.scene_dist, 10.0) / 10.0, -ray.dir.y, -ray.dir.z), 0.0, ray.near);
            }`,
    });
    img = scene.captureFrame();
    const dist = Math.hypot(2.2, 3.2), dir = [0, 2.2 / dist, 3.2 / dist];
    const c = px(img, S / 2, S / 2);
    assert(Math.abs(c[0] - 255 * dist / 10) <= 3, `scene_dist is the plane's distance at the centre (${c[0]} vs ${(255 * dist / 10).toFixed(1)})`);
    assert(Math.abs(c[1] - 255 * dir[1]) <= 4 && Math.abs(c[2] - 255 * dir[2]) <= 4,
           `dir is the unit ray (${c[1]}, ${c[2]} vs ${(255 * dir[1]).toFixed(1)}, ${(255 * dir[2]).toFixed(1)})`);
    assert(px(img, S / 2, 2)[0] === 255, 'over the sky scene_dist is huge');
    scene.destroyNode(probeVol);

    // The near grid: a medium given point by point (medium) to its range,
    // read by each pixel to its opaque depth, the march (volume) on from
    // ray.near seen through it.
    const nearVol = scene.createVolume({
        module: `
            module hook;
            import volume;
            struct Params {
                range: f32,
                sigma: f32,
                scatter: f32,
                parts: f32,
            }
            @binding(4, 0) var params: uniform<Params>;
            pub fn medium_range() -> f32 {
                return params.range;
            }
            pub fn medium(pos: vec3<f32>, dir: vec3<f32>, footprint: f32) -> volume::Medium {
                let q = vec3<f32>(params.scatter);
                let ext = select(params.parts > 1.5, vec4<f32>(params.sigma, 0.0, 0.0, 0.0), vec4<f32>(params.sigma));
                return volume::Medium {
                    scatter0: q, scatter1: q, scatter2: q, scatter3: q,
                    extinction: ext, clear: 0.0,
                };
            }
            pub fn volume(ray: volume::Ray) -> volume::Light {
                let at_range = step(abs(ray.near - params.range), 0.001);
                return volume::light(vec3<f32>(0.0, 0.0, 0.2 * at_range), 1.0, ray.near);
            }`,
        uniforms: { range: 2, sigma: 0.5, scatter: 0.25, parts: 1 },
    });
    const uniform = (d, behind) => {
        const t = Math.exp(-0.5 * d), l = 0.5 * (1 - t);
        return behind.map((b, i) => 255 * (b * t + l + (i === 2 ? 0.2 * t : 0)));
    };
    const glow = [0.6, 0.2, 0.1];
    img = scene.captureFrame();
    let want = uniform(2, glow), got = px(img, S / 2, S / 2);
    assert(near(got, want, 3), `over the plane past a 2 m range: the medium's 2 m, then the march (${got} vs ${want.map(Math.round)})`);
    want = uniform(2, [0, 0, 0]);
    got = px(img, S / 2, 2);
    assert(near(got, want, 3), `over the sky the medium to its range (${got} vs ${want.map(Math.round)})`);
    nearVol.setShaderUniform('range', 10);
    img = scene.captureFrame();
    want = uniform(dist, glow);
    got = px(img, S / 2, S / 2);
    assert(near(got, want, 3), `a range past the plane is read to the plane (${got} vs ${want.map(Math.round)})`);
    nearVol.setShaderUniform('sigma', 50);
    nearVol.setShaderUniform('scatter', 0);
    nearVol.setShaderUniform('parts', 4);
    img = scene.captureFrame();
    got = px(img, S / 2, S / 2);
    want = glow.map((b, i) => 255 * 0.75 * (b + (i === 2 ? 0.2 : 0)));
    assert(near(got, want, 3), `a dense medium in one part of four hides a quarter (${got} vs ${want.map(Math.round)})`);
    scene.destroyNode(nearVol);

    threw = null;
    try {
        scene.createVolume({
            module: `
                module hook;
                import volume;
                pub fn medium(p: vec3<f32>, d: vec3<f32>, w: f32) -> volume::Medium {
                    let z = vec3<f32>(0.0);
                    return volume::Medium { scatter0: z, scatter1: z, scatter2: z, scatter3: z, extinction: vec4<f32>(0.0), clear: 0.0 };
                }
                pub fn volume(ray: volume::Ray) -> volume::Light {
                    return volume::light(vec3<f32>(0.0), 1.0, ray.near);
                }`,
        });
    } catch (e) { threw = e; }
    assert(threw !== null, 'a medium without medium_range is refused');

    // The reconstruction: a medium whose light is noise, another value
    // each pixel and each render (camera.frame.x), lands as its average
    // over renders.
    const noisy = scene.createVolume({
        module: `
            module hook;
            import scene;
            import volume;
            pub fn volume(ray: volume::Ray) -> volume::Light {
                var h: u32 = (ray.pixel.x as u32) * 1973u + (ray.pixel.y as u32) * 9277u + (scene::camera.frame.x as u32) * 26699u;
                h = (h ^ (h >> 13u)) * 0x5bd1e995u;
                let n = ((h ^ (h >> 15u)) & 1023u) as f32 / 1023.0;
                return volume::light(vec3<f32>(n), 0.0, ray.near);
            }`,
    });
    const spread = (im) => {
        let s = 0, s2 = 0, n = 0;
        for (let y = 16; y < 48; y++) for (let x = 16; x < 48; x++) {
            const v = px(im, x, y)[0];
            s += v; s2 += v * v; n++;
        }
        return { mean: s / n, dev: Math.sqrt(Math.max(s2 / n - (s / n) * (s / n), 0)) };
    };
    const first = spread(scene.captureFrame());
    let settled = first;
    for (let i = 0; i < 40; i++) settled = spread(scene.captureFrame());
    assert(first.dev > 60, `a volume's first render is its own (deviation ${first.dev.toFixed(1)})`);
    assert(settled.dev < first.dev / 2 && Math.abs(settled.mean - 127.5) < 12,
           `per-render noise averages over renders (deviation ${first.dev.toFixed(1)} -> ${settled.dev.toFixed(1)}, mean ${settled.mean.toFixed(1)})`);
    scene.destroyNode(noisy);

    threw = null;
    try {
        scene.createVolume({
            module: `
                module hook;
                import mesh;
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface { return s; }`,
        });
    } catch (e) { threw = e; }
    assert(threw !== null, 'a module without volume is refused');
    threw = null;
    try { scene.createVolume({}); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'createVolume without a module throws a TypeError');

    // The incident meter reads the volumes.
    const meterScene = (intensity, ground) => {
        const sc = freshScene(S).scene;
        sc.setAtmosphere({});
        sc.setExposure({ mode: 'auto', meter: 'incident' });
        sc.createLight({ type: 'directional', direction: [-0.3, -0.8, -0.5], color: [1, 1, 1], intensity });
        sc.createMesh({ mesh: 'plane', color: ground, scale: [80, 1, 80] });
        const vol = sc.createVolume({
            module: `
                module hook;
                import lighting;
                import volume;
                struct Params {
                    sun_t: f32,
                    up_t: f32,
                    down: f32,
                }
                @binding(4, 0) var params: uniform<Params>;
                pub fn volume(ray: volume::Ray) -> volume::Light {
                    var rad = vec3<f32>(0.0);
                    var trans = 1.0;
                    if (ray.pixel.y <= 1.0 && ray.pixel.x <= 4.0) {
                        if (dot(ray.dir, lighting::to_sun()) > 0.998) {
                            trans = params.sun_t;
                        } else if (ray.dir.y > 0.0) {
                            rad = vec3<f32>(params.down);
                            trans = params.up_t;
                        }
                    }
                    return volume::light(rad, trans, ray.near);
                }`,
            uniforms: { sun_t: 1.0, up_t: 1.0, down: 0.0 },
        });
        const metered = (sunT, upT, down) => {
            vol.setShaderUniform('sun_t', sunT);
            vol.setShaderUniform('up_t', upT);
            vol.setShaderUniform('down', down);
            for (let i = 0; i < 6; i++) sc.captureFrame();
            return sc.cullStats().meteredEv;
        };
        return metered;
    };
    const lux = (ev) => Math.pow(2, ev) * 1.2 * Math.PI;
    const day = meterScene(100000, '#121212');
    const clear = day(1, 1, 0);
    const both = day(0.5, 0.5, 0);
    assert(Math.abs(clear - both - 1) < 0.1,
           `half the sun and half the sky passed meter a stop darker (${clear.toFixed(2)} -> ${both.toFixed(2)})`);
    const half = day(0.5, 1, 0);
    const skyLux = 2 * (lux(half) - lux(both)), sunLux = lux(clear) - skyLux;
    assert(skyLux > 0.02 * lux(clear) && sunLux > 0,
           `the sky's light counts beside the sun's (${lux(clear).toFixed(0)} lux, the sky ${skyLux.toFixed(0)})`);
    const overcast = day(0.25, 1, 0.25 * sunLux / Math.PI);
    assert(Math.abs(overcast - half) < 0.1,
           `the volume's light down counts as an overcast sky's (${overcast.toFixed(2)}, half ${half.toFixed(2)})`);
    {
        const sc = freshScene(S).scene;
        sc.setAtmosphere({});
        sc.setExposure({ mode: 'auto', meter: 'incident' });
        sc.createLight({ type: 'directional', direction: [-0.3, -0.8, -0.5], color: [1, 1, 1], intensity: 100000 });
        sc.createMesh({ mesh: 'plane', color: '#121212', scale: [80, 1, 80] });
        const vol = sc.createVolume({
            module: `
                module hook;
                import volume;
                struct Params { wide: f32 }
                @binding(4, 0) var params: uniform<Params>;
                pub fn volume(ray: volume::Ray) -> volume::Light {
                    let w = volume::footprint(ray, 1.0);
                    var rad = vec3<f32>(0.0);
                    var trans = 1.0;
                    if (ray.pixel.y > 1.0 || ray.pixel.x > 4.0) {
                        if (w > 0.1) {
                            rad = vec3<f32>(1000.0, 0.0, 0.0);
                            trans = 0.0;
                        }
                    } else if (params.wide > 0.5 && w > 1.0) {
                        trans = 0.5;
                    }
                    return volume::light(rad, trans, ray.near);
                }`,
            uniforms: { wide: 0.0 },
        });
        const ev = (wide) => {
            vol.setShaderUniform('wide', wide);
            for (let i = 0; i < 6; i++) sc.captureFrame();
            return sc.cullStats().meteredEv;
        };
        const open = ev(0), wide = ev(1);
        assert(Math.abs(open - wide - 1) < 0.1,
               `the meter's footprint is the scene's scale: half passed over it meters a stop darker (${open.toFixed(2)} -> ${wide.toFixed(2)})`);
        const img = sc.captureFrame();
        const c = px(img, S / 2, S / 2), top = px(img, S / 2, 2);
        assert(!(c[0] > 200 && c[1] < 50) && !(top[0] > 200 && top[1] < 50),
               `a pixel's footprint a metre out is a pixel's cone (${c}, ${top})`);
    }
    console.log('volume tests passed');
}

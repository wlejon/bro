// scene.createVolume: a medium over the whole view, its chunk's userVolume
// marching each pixel's ray up to the opaque depth, composited as
// scene * transmittance + its light.
//   - the composite: an emissive plane and the black sky behind a medium of
//     transmittance 0.5 and light 0.2 read plane * 0.5 + 0.2 and 0.2;
//   - sceneDist is the opaque surface's distance along the ray (the camera
//     to the plane's centre at the view's centre), and huge over the sky;
//   - dir is the unit ray (the view's centre looks along -camera);
//   - setShaderUniform reaches the chunk; a volume casts no shadow and does
//     not hide what it covers from the depth;
//   - a chunk without userVolume, or with a vertex chunk, is refused.

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
        fragment: `uniform vec3 u_glow;
            void userFragment(inout vec3 baseColor, inout vec3 normal, inout float metallic,
                              inout float roughness, inout vec3 emissive, inout float alpha) {
                baseColor = vec3(0.0);
                metallic = 0.0;
                roughness = 1.0;
                emissive = u_glow;
            }`,
        uniforms: { u_glow: rgb },
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
    plane(scene, [0.6, 0.2, 0.1]);
    const before = scene.captureFrame();
    const centre = px(before, S / 2, S / 2), sky = px(before, S / 2, 2);
    assert(near(centre, [153, 51, 26], 3) && near(sky, [0, 0, 0], 1),
           `the plane glows and the sky is black (${centre}, ${sky})`);

    const fog = scene.createVolume({
        fragment: `uniform float u_t;
            uniform vec3 u_light;
            void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
                            inout float transmittance, inout float depth) {
                transmittance = u_t;
                radiance = u_light;
            }`,
        uniforms: { u_t: 0.5, u_light: [0.2, 0.2, 0.2] },
    });
    assert(fog && fog.hasShader, 'createVolume makes a node with its chunk');
    let img = scene.captureFrame();
    assert(near(px(img, S / 2, S / 2), [127, 76, 64], 3),
           `over the plane: plane * 0.5 + 0.2 (${px(img, S / 2, S / 2)})`);
    assert(near(px(img, S / 2, 2), [51, 51, 51], 2), `over the sky: 0.2 (${px(img, S / 2, 2)})`);

    fog.setShaderUniform('u_t', 1.0);
    fog.setShaderUniform('u_light', [0, 0, 0]);
    img = scene.captureFrame();
    assert(near(px(img, S / 2, S / 2), centre, 2) && near(px(img, S / 2, 2), sky, 1),
           'a clear medium leaves the scene as it was (setShaderUniform reaches the chunk)');
    scene.destroyNode(fog);

    // The distance to the opaque surface and the ray, as colours.
    const probeVol = scene.createVolume({
        fragment: `void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
                                   inout float transmittance, inout float depth) {
                transmittance = 0.0;
                radiance = vec3(min(sceneDist, 10.0) / 10.0, -dir.y, -dir.z);
            }`,
    });
    img = scene.captureFrame();
    const dist = Math.hypot(2.2, 3.2), dir = [0, 2.2 / dist, 3.2 / dist];
    const c = px(img, S / 2, S / 2);
    assert(Math.abs(c[0] - 255 * dist / 10) <= 3, `sceneDist is the plane's distance at the centre (${c[0]} vs ${(255 * dist / 10).toFixed(1)})`);
    assert(Math.abs(c[1] - 255 * dir[1]) <= 4 && Math.abs(c[2] - 255 * dir[2]) <= 4,
           `dir is the unit ray (${c[1]}, ${c[2]} vs ${(255 * dir[1]).toFixed(1)}, ${(255 * dir[2]).toFixed(1)})`);
    assert(px(img, S / 2, 2)[0] === 255, 'over the sky sceneDist is huge');
    scene.destroyNode(probeVol);

    // The reconstruction: a medium whose light is noise, another value
    // each pixel and each render (camera.frame.x), lands as its average
    // over renders. The first render of a volume is its own alone (the
    // noise's full spread), and over renders the pixels gather toward the
    // mean, 0.5.
    const noisy = scene.createVolume({
        fragment: `void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
                                   inout float transmittance, inout float depth) {
                uint h = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + uint(camera.frame.x) * 26699u;
                h = (h ^ (h >> 13)) * 0x5bd1e995u;
                transmittance = 0.0;
                radiance = vec3(float((h ^ (h >> 15)) & 1023u) / 1023.0);
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

    let threw = null;
    try {
        scene.createVolume({ fragment: 'void userFragment(inout vec3 a, inout vec3 b, inout float c, inout float d, inout vec3 e, inout float f) {}' });
    } catch (e) { threw = e; }
    assert(threw !== null, 'a chunk without userVolume is refused');
    threw = null;
    try { scene.createVolume({}); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'createVolume without a fragment chunk throws a TypeError');

    // The incident meter reads the volumes: one that passes a share of the
    // sun within a few degrees of it and a share of the sky over the rest of
    // the hemisphere, sending light down evenly from it, only at the frame's
    // first pixels (where the meter's rays are marched), leaves the frame as
    // it was and moves the meter alone.
    // The meter reads the sun and the sky's light on a level surface; above
    // a grey card's 3 cd/m^2 an EV is log2(E / (1.2 pi)), so E goes as 2^EV.
    const meterScene = (intensity, ground) => {
        const sc = freshScene(S).scene;
        sc.setAtmosphere({});
        sc.setExposure({ mode: 'auto', meter: 'incident' });
        sc.createLight({ type: 'directional', direction: [-0.3, -0.8, -0.5], color: [1, 1, 1], intensity });
        sc.createMesh({ mesh: 'plane', color: ground, scale: [80, 1, 80] });
        const vol = sc.createVolume({
            fragment: `uniform float u_sunT;
                uniform float u_upT;
                uniform float u_down;
                void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
                                inout float transmittance, inout float depth) {
                    if (gl_FragCoord.y > 1.0 || gl_FragCoord.x > 4.0) return;
                    if (dot(dir, sceneToSun()) > 0.998) transmittance = u_sunT;
                    else if (dir.y > 0.0) { radiance = vec3(u_down); transmittance = u_upT; }
                }`,
            uniforms: { u_sunT: 1.0, u_upT: 1.0, u_down: 0.0 },
        });
        const metered = (sunT, upT, down) => {
            vol.setShaderUniform('u_sunT', sunT);
            vol.setShaderUniform('u_upT', upT);
            vol.setShaderUniform('u_down', down);
            for (let i = 0; i < 6; i++) sc.captureFrame();
            return sc.cullStats().meteredEv;
        };
        return metered;
    };
    const lux = (ev) => Math.pow(2, ev) * 1.2 * Math.PI;
    // A day's sun over darkish ground: the incident reading sits a stop and a
    // half above the frame's, so every reading below stays inside the reach
    // about it the meter keeps (the volume moves the meter, not the pixels).
    const day = meterScene(100000, '#121212');
    const clear = day(1, 1, 0);
    const both = day(0.5, 0.5, 0);
    assert(Math.abs(clear - both - 1) < 0.1,
           `half the sun and half the sky passed meter a stop darker (${clear.toFixed(2)} -> ${both.toFixed(2)})`);
    // Half the sun alone: what the sky passed adds back half the sky's.
    const half = day(0.5, 1, 0);
    const skyLux = 2 * (lux(half) - lux(both)), sunLux = lux(clear) - skyLux;
    assert(skyLux > 0.02 * lux(clear) && sunLux > 0,
           `the sky's light counts beside the sun's (${lux(clear).toFixed(0)} lux, the sky ${skyLux.toFixed(0)})`);
    // Light down of L from the whole hemisphere adds pi L: matching a quarter
    // of the sun, a quarter of the sun passed meters as half.
    const overcast = day(0.25, 1, 0.25 * sunLux / Math.PI);
    assert(Math.abs(overcast - half) < 0.1,
           `the volume's light down counts as an overcast sky's (${overcast.toFixed(2)}, half ${half.toFixed(2)})`);
    // Dusk: below the knee a quarter of the light is a stop darker, not two.
    const dusk = meterScene(30, '#121212');
    const dim = dusk(1, 1, 0), dimmer = dusk(0.25, 0.25, 0);
    assert(dim < 3.8 && Math.abs(dim - dimmer - 1) < 0.1,
           `below a grey card's 3 cd/m^2 each stop of light is half a stop (${dim.toFixed(2)} -> ${dimmer.toFixed(2)})`);
}

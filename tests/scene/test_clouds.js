// The cloud deck (scene.setClouds, scene/world/clouds.h,
// vulkan/sky/scene_clouds.cpp): a world's density module in a shell over
// the body, marched per pixel and casting its shadow on the land.
// Verifies:
//   - a deck whose module is all clear draws exactly what no deck draws,
//     looking up at the sky and down at the land
//   - an overcast deck greys the blue sky over the eye
//   - its shadow darkens the sunlit land under it
//   - the same deck at the same time draws the same picture twice
//   - cloudStats: compiled and covered once drawn; a module that does not
//     compile reports failed and the scene still draws; setClouds(null)
//     takes the deck away (the picture is the deckless one again)

function boxRGB(img, x0, y0, x1, y1) {
    const s = [0, 0, 0];
    let n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            s[0] += img.data[i]; s[1] += img.data[i + 1]; s[2] += img.data[i + 2]; n++;
        }
    return s.map((v) => v / n);
}
const fmt = (c) => '[' + c.map((v) => v.toFixed(1)).join(', ') + ']';
function differing(a, b) {
    let n = 0;
    for (let i = 0; i < a.data.length; i++) if (a.data[i] !== b.data[i]) n++;
    return n;
}

const N = (type, props = {}) => {
    const n = FastNoise.create(type);
    for (const k in props) n.set(k, props[k]);
    return n;
};

const S = 192;
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
document.body.appendChild(canvas);
flush();

const scn = canvas.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    const frames = (n = 3) => { flush(); for (let i = 0; i < n; i++) advanceTime(16); };
    const R = 6371000;
    scn.setBody({ radius: R, at: [0, R, 0] });
    scn.setToneMap({ mode: 'aces' });
    scn.setDither(false);
    scn.createLight({ type: 'directional', direction: [-0.3, -0.8, -0.5], color: [1, 1, 1], intensity: 3 });
    scn.setAtmosphere({});
    const land = scn.createLand({ height: N('Constant', { Value: 0 }),
                                  surface: 'LandSurface landSurface(LandPoint q) { return landSurfaceOf(vec3(0.2, 0.3, 0.12)); }' });
    assert(land, 'createLand on the body');

    const up = () => scn.setCamera({ fov: 60, near: 0.1, far: 1e6, position: [0, 2, 0], target: [0, 6, -5], up: [0, 1, 0] });
    const down = () => scn.setCamera({ fov: 60, near: 0.1, far: 1e6, position: [0, 40, 0], target: [0, 0, -60], up: [0, 1, 0] });
    const shoot = (view) => { view(); frames(12); return scn.captureFrame(); };

    const offUp = shoot(up), offDown = shoot(down);
    const skyOff = boxRGB(offUp, 32, 16, 160, 96);
    const groundOff = boxRGB(offDown, 32, 120, 160, 184);
    console.log(`no deck: sky ${fmt(skyOff)} ground ${fmt(groundOff)}`);
    assert(skyOff[2] > skyOff[0] + 20, `without a deck the sky is blue ${fmt(skyOff)}`);

    // --- a deck that is all clear ----------------------------------------------------
    scn.setClouds({ module: 'vec4 cloudCover(vec3 up) { return vec4(1.0); }\nfloat cloudDensity(CloudPoint q) { return 0.0; }',
                    bottom: 1500, top: 3000 });
    const clearUp = shoot(up), clearDown = shoot(down);
    const st = scn.cloudStats();
    console.log(`clear deck stats ${JSON.stringify(st)}`);
    assert(st.enabled && st.compiled && !st.failed && st.covered, `a clear deck compiles and is covered ${JSON.stringify(st)}`);
    assert(differing(clearUp, offUp) === 0, `a clear deck draws the sky as no deck does (${differing(clearUp, offUp)} bytes differ)`);
    assert(differing(clearDown, offDown) === 0, `a clear deck draws the land as no deck does (${differing(clearDown, offDown)} bytes differ)`);

    // --- an overcast deck ------------------------------------------------------------
    scn.setClouds({ module: 'vec4 cloudCover(vec3 up) { return vec4(1.0); }\nfloat cloudDensity(CloudPoint q) { return 0.02; }',
                    bottom: 1500, top: 3000 });
    const overUp = shoot(up), overDown = shoot(down);
    const skyOver = boxRGB(overUp, 32, 16, 160, 96);
    const groundOver = boxRGB(overDown, 32, 120, 160, 184);
    console.log(`overcast: sky ${fmt(skyOver)} ground ${fmt(groundOver)}`);
    assert(Math.abs(skyOver[2] - skyOver[0]) < 0.5 * (skyOff[2] - skyOff[0]),
           `under an overcast deck the sky is grey, not blue ${fmt(skyOver)} (clear ${fmt(skyOff)})`);
    const lum = (c) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    assert(lum(groundOver) < 0.8 * lum(groundOff),
           `the deck's shadow darkens the land ${fmt(groundOver)} (sunlit ${fmt(groundOff)})`);

    // --- deterministic ---------------------------------------------------------------
    const again = shoot(up);
    assert(differing(again, overUp) === 0, `the same deck draws the same picture twice (${differing(again, overUp)} bytes differ)`);

    // --- a module that does not compile ----------------------------------------------
    scn.setClouds({ module: 'float cloudDensity(CloudPoint q) { return nope; }', bottom: 1500, top: 3000 });
    const broken = shoot(up);
    const bs = scn.cloudStats();
    assert(bs.failed && !bs.compiled, `a module that does not compile reports failed ${JSON.stringify(bs)}`);
    assert(differing(broken, offUp) === 0, `with a failed module the scene draws as without a deck`);

    // --- taken away --------------------------------------------------------------------
    scn.setClouds(null);
    const gone = shoot(up);
    assert(!scn.cloudStats().enabled, 'setClouds(null) takes the deck away');
    assert(differing(gone, offUp) === 0, `without the deck again the sky is as it was (${differing(gone, offUp)} bytes differ)`);

    let threw = false;
    try { scn.setClouds({ module: 'x', bottom: 3000, top: 1000 }); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'a deck whose top is under its bottom is a TypeError');

    console.log('test_clouds: OK');
}

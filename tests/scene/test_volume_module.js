// scene.createVolume({ module }): a volume whose hook is a brokernel module
// (volume.bk's contract), composed with the engine's modules and compiled by
// brass, drawing as a GLSL chunk's does.
//   - the composite: an emissive plane and the black sky behind a medium of
//     transmittance 0.5 and light 0.2 read plane * 0.5 + 0.2 and 0.2;
//   - setShaderUniform reaches the module's uniform by its field's name;
//   - ray.scene_dist is the opaque surface's distance, ray.dir the unit ray;
//   - a medium (medium_range, medium) is integrated by the near grid to the
//     opaque depth or its range, and the march starts at its range;
//   - setShaderBuffer reaches a read_buffer of set 4, an unbound one len() 0;
//   - `modules` gives the hook the app's modules to import (an engine
//     module's name refused), and ray.pixel is the pixel's centre;
//   - a module that does not compile, or declares a set-4 resource the set
//     does not hold, is refused with the compiler's message.

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

const FOG = `
module hook;
import volume;

struct Params {
    t: f32,
    light: vec3<f32>,
}
@binding(4, 0) var params: uniform<Params>;

pub fn volume(ray: volume::Ray) -> volume::Light {
    return volume::light(params.light, params.t, ray.near);
}
`;

const PROBE = `
module hook;
import volume;

pub fn volume(ray: volume::Ray) -> volume::Light {
    return volume::light(vec3<f32>(min(ray.scene_dist, 10.0) / 10.0, -ray.dir.y, -ray.dir.z), 0.0, ray.near);
}
`;

// A uniform medium of extinction sigma and scatter q a metre to its range;
// the march adds blue 0.2 when it starts at the range.
const MEDIUM = `
module hook;
import volume;

struct Params {
    range: f32,
    sigma: f32,
    scatter: f32,
}
@binding(4, 0) var params: uniform<Params>;

pub fn medium_range() -> f32 {
    return params.range;
}

pub fn medium(pos: vec3<f32>, dir: vec3<f32>, footprint: f32) -> volume::Medium {
    let q = vec3<f32>(params.scatter);
    return volume::Medium { scatter0: q, scatter1: q, scatter2: q, scatter3: q,
                            extinction: vec4<f32>(params.sigma), clear: 0.0 };
}

pub fn volume(ray: volume::Ray) -> volume::Light {
    let at_range = step(abs(ray.near - params.range), 0.001);
    return volume::light(vec3<f32>(0.0, 0.0, 0.2 * at_range), 1.0, ray.near);
}
`;

// Its light the first element of a buffer, black with none bound.
const BUFFER = `
module hook;
import volume;

@binding(4, 14) var tint: read_buffer<vec4<f32>>;

pub fn volume(ray: volume::Ray) -> volume::Light {
    var c = vec3<f32>(0.0);
    if (tint.len() > 0) {
        c = tint[0].xyz;
    }
    return volume::light(c, 0.0, ray.near);
}
`;

// Its light the pixel it is marched in over the frame's size, from a module
// of the app's.
const SHADE = `
module app::shade;

pub fn of_pixel(p: vec2<f32>, size: f32) -> vec3<f32> {
    return vec3<f32>(p.x / size, p.y / size, 0.0);
}
`;
const PIXEL = `
module hook;
import volume;
from app::shade import of_pixel;

pub fn volume(ray: volume::Ray) -> volume::Light {
    return volume::light(of_pixel(ray.pixel, 64.0), 0.0, ray.near);
}
`;

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

    const fog = scene.createVolume({ module: FOG, file: 'fog.bk', uniforms: { t: 0.5, light: [0.2, 0.2, 0.2] } });
    assert(fog && fog.hasShader, 'createVolume makes a node with its module');
    let img = scene.captureFrame();
    assert(near(px(img, S / 2, S / 2), [127, 76, 64], 3),
           `over the plane: plane * 0.5 + 0.2 (${px(img, S / 2, S / 2)})`);
    assert(near(px(img, S / 2, 2), [51, 51, 51], 2), `over the sky: 0.2 (${px(img, S / 2, 2)})`);
    fog.setShaderUniform('t', 1.0);
    fog.setShaderUniform('light', [0, 0, 0]);
    img = scene.captureFrame();
    assert(near(px(img, S / 2, S / 2), centre, 2) && near(px(img, S / 2, 2), sky, 1),
           'a clear medium leaves the scene as it was (setShaderUniform reaches the field)');
    scene.destroyNode(fog);

    const probeVol = scene.createVolume({ module: PROBE, file: 'probe.bk' });
    img = scene.captureFrame();
    const dist = Math.hypot(2.2, 3.2), dir = [0, 2.2 / dist, 3.2 / dist];
    const c = px(img, S / 2, S / 2);
    assert(Math.abs(c[0] - 255 * dist / 10) <= 3, `scene_dist is the plane's distance at the centre (${c[0]} vs ${(255 * dist / 10).toFixed(1)})`);
    assert(Math.abs(c[1] - 255 * dir[1]) <= 4 && Math.abs(c[2] - 255 * dir[2]) <= 4,
           `dir is the unit ray (${c[1]}, ${c[2]} vs ${(255 * dir[1]).toFixed(1)}, ${(255 * dir[2]).toFixed(1)})`);
    assert(px(img, S / 2, 2)[0] === 255, 'over the sky scene_dist is huge');
    scene.destroyNode(probeVol);

    const nearVol = scene.createVolume({ module: MEDIUM, file: 'medium.bk',
                                         uniforms: { range: 2, sigma: 0.5, scatter: 0.25 } });
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
    scene.destroyNode(nearVol);

    const tinted = scene.createVolume({ module: BUFFER, file: 'buffer.bk' });
    img = scene.captureFrame();
    assert(near(px(img, S / 2, 2), [0, 0, 0], 1), `an unbound buffer is empty (${px(img, S / 2, 2)})`);
    const buf = bro.kernel.createBuffer(new Float32Array([0.4, 0.2, 0.1, 1.0]));
    tinted.setShaderBuffer('tint', buf);
    img = scene.captureFrame();
    assert(near(px(img, S / 2, 2), [102, 51, 26], 2), `setShaderBuffer reaches the module's buffer (${px(img, S / 2, 2)})`);
    scene.destroyNode(tinted);

    const pixelVol = scene.createVolume({ module: PIXEL, file: 'pixel.bk', modules: { 'app::shade': SHADE } });
    img = scene.captureFrame();
    want = [255 * (S / 2 + 0.5) / S, 255 * 2.5 / S, 0];
    got = px(img, S / 2, 2);
    assert(near(got, want, 2), `a module of the app's is imported, and ray.pixel is the pixel's centre (${got} vs ${want.map(Math.round)})`);
    scene.destroyNode(pixelVol);

    let threw = null;
    try {
        scene.createVolume({ module: PIXEL, file: 'pixel.bk', modules: { 'app::shade': SHADE, scene: SHADE } });
    } catch (e) { threw = e; }
    assert(threw !== null && String(threw.message).includes("'scene' is the engine's"), `an engine module's name is refused (${threw && threw.message})`);
    threw = null;
    try {
        scene.createVolume({ module: 'module hook;\npub fn volume(ray: volume::Ray) -> volume::Light { return 1; }', file: 'bad.bk' });
    } catch (e) { threw = e; }
    assert(threw !== null && String(threw.message).includes('bad.bk'), `a module that does not compile is refused (${threw && threw.message})`);
    threw = null;
    try {
        scene.createVolume({ module: FOG.replace('@binding(4, 0) var params', '@binding(4, 9) var params'), file: 'wrong.bk' });
    } catch (e) { threw = e; }
    assert(threw !== null && String(threw.message).includes('(4, 9)'), `a resource set 4 does not hold is refused (${threw && threw.message})`);
}

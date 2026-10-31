// scene.createMesh({ module }) / node.setShaderModule(source, file, modules):
// a mesh whose hook is a brokernel module (mesh.bk's contract), composed with
// the engine's modules and compiled by brass, drawing in both the main pass
// and the shadow pass.
//   - fragment(v, s) -> mesh::Surface overrides PBR inputs and emissive;
//   - vertex(v) -> mesh::Vertex displaces the mesh and its shadow silhouette;
//   - light(v, s, l) -> mesh::Light scales sun/sky/occluder/diffuse lighting;
//   - setShaderUniform reaches the module's uniform by field name;
//   - setShaderTexture reaches custom::sample(slot, uv) at set 4 bindings 1..8;
//   - setShaderBuffer reaches a read_buffer of set 4 (bindings 14..19);
//   - `modules` gives the hook the app's modules to import (an engine
//     module's name is refused);
//   - a module that does not compile, or declares a set-4 resource the set
//     does not hold, is refused with the compiler's message and leaves the
//     previous shader intact.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setDither(false);
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0, 0, 0] });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

function regionChannel(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

function centerChannel(img, ch, r) {
    const cx = Math.floor(img.width / 2), cy = Math.floor(img.height / 2);
    return regionChannel(img, ch, cx - r, cy - r, cx + r, cy + r);
}

const EMIT_MOD = `
module hook;
import mesh;

struct Params {
    tint: vec3<f32>,
    gain: f32,
}
@binding(4, 0) var params: uniform<Params>;

pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.0);
    out.metallic = 0.0;
    out.roughness = 1.0;
    out.emissive = params.tint * params.gain;
    return out;
}
`;

const DISPLACE_MOD = `
module hook;
import mesh;

struct Params {
    dx: f32,
}
@binding(4, 0) var params: uniform<Params>;

pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
    var out = v;
    out.pos = vec3<f32>(v.pos.x + params.dx, v.pos.y, v.pos.z);
    return out;
}
`;

const LIGHT_MOD = `
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
}
`;

const TEX_BUF_MOD = `
module hook;
import mesh;
import custom;

@binding(4, 1) var tex0: texture2d<f32>;
@binding(4, 14) var buf_tint: read_buffer<vec4<f32>>;

pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.0);
    out.metallic = 0.0;
    out.roughness = 1.0;
    let tex = tex0.sample(custom::linear, vec2<f32>(0.5, 0.5));
    var b = vec3<f32>(1.0);
    if (buf_tint.len() > 0) {
        b = buf_tint[0].xyz;
    }
    out.emissive = b * tex;
    return out;
}
`;

const APP_HELPER = `
module app::tint;

pub fn scale_rgb(c: vec3<f32>, k: f32) -> vec3<f32> {
    return c * k;
}
`;

const APP_IMPORT_MOD = `
module hook;
import mesh;
from app::tint import scale_rgb;

pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.0);
    out.roughness = 1.0;
    out.emissive = scale_rgb(vec3<f32>(0.4, 0.8, 0.2), 0.5);
    return out;
}
`;

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 128;

    // 1. Fragment hook + setShaderUniform by field name + clearShader
    {
        const s = freshScene(SIZE);
        const m = s.scene.createMesh({
            mesh: 'box',
            color: '#ff0000',
            module: EMIT_MOD,
            file: 'emit.bk',
            uniforms: { tint: [0, 1, 0], gain: 1.0 },
        });
        assert(m.hasShader === true, 'createMesh({ module }) installs the module');
        let img = s.scene.captureFrame();
        assert(centerChannel(img, 1, 8) > 200 && centerChannel(img, 0, 8) < 20,
               `fragment hook emits green (${centerChannel(img, 1, 8)})`);

        m.setShaderUniform('tint', [0, 0, 1]);
        img = s.scene.captureFrame();
        assert(centerChannel(img, 2, 8) > 200 && centerChannel(img, 1, 8) < 20,
               `setShaderUniform updates struct field live (${centerChannel(img, 2, 8)})`);

        m.setShaderUniform('gain', 0.0);
        img = s.scene.captureFrame();
        assert(centerChannel(img, 2, 8) < 10, 'scalar field update applies');

        s.scene.createLight({ type: 'directional', direction: [0, 0, -1], intensity: 2.0 });
        m.clearShader();
        assert(m.hasShader === false, 'clearShader removes module');
        img = s.scene.captureFrame();
        assert(centerChannel(img, 0, 8) > 60 && centerChannel(img, 2, 8) < 20,
               'clearShader restores default red box');
        dropScene(s);
    }

    // 2. Vertex hook displaces both the mesh and its shadow silhouette
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 8, 0], target: [0, 0, 0], up: [0, 0, -1] });
        sc.createMesh({ mesh: 'plane', halfW: 6, halfD: 6, color: 'white', castsShadow: false });
        const sun = sc.createLight({ type: 'directional', direction: [0, -1, 0], color: [1, 1, 1], intensity: 2 });
        sun.castsShadow = true;

        // Small box at y = 2 above the ground, displaced +2.0 in X by the vertex hook.
        // Without the hook, the box and its shadow are at x = 0 (px 64).
        // With the hook, the box is hidden from camera view by making its fragment alpha=0
        // Wait: let's place the sun angled along +Z (direction [0, -1, 1]) so the shadow lands at z = +2 (py ~ 92),
        // while the box itself is at z = 0 (py ~ 64), and displace the box by dx = +1.8 (px ~ 89).
        sun.direction = [0, -1, 1];
        const caster = sc.createMesh({
            mesh: 'box', halfW: 0.4, halfH: 0.4, halfD: 0.4,
            y: 2.0, color: [1, 0, 0],
            module: DISPLACE_MOD,
            file: 'displace.bk',
            uniforms: { dx: 0.0 },
        });
        caster.cullMargin = 2.0;
        let img = sc.captureFrame();
        // At dx = 0, shadow is at x = 0, z = +2 -> px ~ 64, py ~ 92.
        const shadowCenter0 = regionChannel(img, 0, 60, 88, 68, 96);
        const shadowRight0  = regionChannel(img, 0, 85, 88, 93, 96);
        assert(shadowCenter0 < shadowRight0 * 0.6,
               `before displacement, shadow is at center (${shadowCenter0.toFixed(0)} vs ${shadowRight0.toFixed(0)})`);

        caster.setShaderUniform('dx', 1.8);
        img = sc.captureFrame();
        const shadowCenter1 = regionChannel(img, 0, 60, 88, 68, 96);
        const shadowRight1  = regionChannel(img, 0, 85, 88, 93, 96);
        assert(shadowRight1 < shadowCenter1 * 0.6,
               `displaced vertex hook casts displaced shadow on the right (${shadowRight1.toFixed(0)} vs ${shadowCenter1.toFixed(0)})`);
        dropScene(s);
    }

    // 3. Light hook (sun_visibility, sky_visibility, occluder_sun, sun_diffuse)
    {
        const s = freshScene(32);
        const sc = s.scene;
        sc.setCamera({ fov: 40, near: 0.1, far: 100, position: [0, 4, 0.01], target: [0, 0, 0] });
        sc.createLight({ type: 'directional', direction: [0, -1, 0], color: [1, 1, 1], intensity: Math.PI });
        const plane = sc.createMesh({
            positions: new Float32Array([-2, 0, -2, 2, 0, -2, -2, 0, 2, 2, 0, 2]),
            normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
            indices: new Uint32Array([0, 2, 1, 1, 2, 3]),
            color: [1, 1, 1],
            module: LIGHT_MOD,
            file: 'light.bk',
            uniforms: { sun: 1, sky: 1, occ: 0.5, dif: 0 },
        });
        const centre = () => {
            const img = sc.captureFrame();
            return img.data[((16 * img.width) + 16) * 4];
        };
        const lit = centre();
        assert(lit > 120 && lit < 150, `lit surface reads ~0.5 (${lit})`);
        plane.setShaderUniform('sun', 0.0);
        plane.setShaderUniform('sky', 0.0);
        plane.setShaderUniform('occ', 0.0);
        assert(centre() <= 2, 'zero sun/sky/occ is black');
        plane.setShaderUniform('occ', 1.0);
        const want = 255 * 0.5 * 0.5;
        assert(Math.abs(centre() - want) <= 4, `occluder_sun bounce matches rho^2 E / pi (${centre()})`);
        dropScene(s);
    }

    // 4. Set-4 custom texture (custom::sample) + storage buffer (setShaderBuffer) + app modules
    {
        const s = freshScene(64);
        const sc = s.scene;
        const m = sc.createMesh({
            mesh: 'box',
            module: TEX_BUF_MOD,
            file: 'tex_buf.bk',
        });
        m.setShaderTexture('tex0', {
            width: 2, height: 2,
            data: new Float32Array([0.5, 0.5, 0.5, 0.5]),
        });
        const buf = bro.kernel.createBuffer(new Float32Array([0.8, 0.4, 0.2, 1.0]));
        m.setShaderBuffer('buf_tint', buf);
        let img = sc.captureFrame();
        // 0.8*0.5 = 0.4 (102), 0.4*0.5 = 0.2 (51), 0.2*0.5 = 0.1 (26)
        const r = centerChannel(img, 0, 4), g = centerChannel(img, 1, 4), b = centerChannel(img, 2, 4);
        assert(Math.abs(r - 102) <= 4 && Math.abs(g - 51) <= 4 && Math.abs(b - 26) <= 4,
               `custom texture * storage buffer = [102, 51, 26] (got [${r.toFixed(1)}, ${g.toFixed(1)}, ${b.toFixed(1)}])`);

        // App module import
        m.setShaderModule(APP_IMPORT_MOD, 'app_import.bk', { 'app::tint': APP_HELPER });
        img = sc.captureFrame();
        assert(Math.abs(centerChannel(img, 0, 4) - 51) <= 3 &&
               Math.abs(centerChannel(img, 1, 4) - 102) <= 3 &&
               Math.abs(centerChannel(img, 2, 4) - 26) <= 3,
               'app module imported and evaluated');

        // Error diagnostics & previous shader preservation
        let threw = null;
        try {
            m.setShaderModule(APP_IMPORT_MOD, 'app_import.bk', { 'app::tint': APP_HELPER, mesh: APP_HELPER });
        } catch (e) { threw = e; }
        assert(threw !== null && String(threw.message).includes("'mesh' is the engine's"),
               `engine module name refused (${threw && threw.message})`);

        threw = null;
        try {
            m.setShaderModule('module hook;\npub fn fragment(v: mesh::Vertex) -> i32 { return 1; }', 'broken.bk');
        } catch (e) { threw = e; }
        assert(threw !== null && String(threw.message).includes('broken.bk'),
               `invalid module refused with filename (${threw && threw.message})`);

        // Previous shader still works after failed setShaderModule
        img = sc.captureFrame();
        assert(Math.abs(centerChannel(img, 1, 4) - 102) <= 3, 'previous module preserved after failed compile');
        dropScene(s);
    }

    // 5. Instanced and Skinned mesh nodes with .bk modules
    {
        const s = freshScene(64);
        const sc = s.scene;
        const inst = sc.createInstancedMesh({
            mesh: Mesh.box(),
            color: 'red',
            instancesFromTransforms: new Float32Array([0, 0, 0, 0, 0, 0, 1, 1, 0]),
            module: EMIT_MOD,
            file: 'inst_emit.bk',
            uniforms: { tint: [0, 1, 0], gain: 1.0 },
        });
        assert(inst.hasShader === true, 'createInstancedMesh({ module }) installs module');
        let img = sc.captureFrame();
        assert(centerChannel(img, 1, 4) > 200 && centerChannel(img, 0, 4) < 20,
               'instanced module fragment hook renders green');
        inst.visible = false;

        const positions = new Float32Array([-0.5, -0.5, 0,  0.5, -0.5, 0,  0.5, 0.5, 0, -0.5, 0.5, 0]);
        const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]);
        const indices = new Uint32Array([0, 1, 2, 0, 2, 3]);
        const skin = new SkinData({
            boneWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]),
            boneIndices: new Uint32Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            inverseBindMatrices: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
            boneCount: 1,
        });
        const sk = sc.createSkinnedMesh({
            positions, normals, indices, skin,
            color: 'red',
            module: EMIT_MOD,
            file: 'skin_emit.bk',
            uniforms: { tint: [0, 0, 1], gain: 1.0 },
        });
        assert(sk.hasShader === true, 'createSkinnedMesh({ module }) installs module');
        img = sc.captureFrame();
        assert(centerChannel(img, 2, 4) > 200 && centerChannel(img, 0, 4) < 20,
               'skinned module fragment hook renders blue');
        dropScene(s);
    }
}

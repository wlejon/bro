// A culled instanced draw's instances hidden behind what is drawn are left
// out of the camera's view (pass_draw_counts.h), by the frame's own depth:
//  - instances behind a wall do not show, the ones beside it do;
//  - the frame the wall is gone, every instance shows: none is a frame late,
//    however long it was hidden;
//  - the same with the scene multisampled.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 6], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0, 0, 0] });
        sc.createLight({ type: 'directional', intensity: 0 });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

const hook = `
    module hook;
    import mesh;
    @binding(4, 14) var xs: read_buffer<f32>;
    pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
        var out = v;
        out.pos.x += xs[v.instance_index];
        out.v0.x = v.instance_index as f32;
        return out;
    }
    pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
        var out = s;
        let k = v.v0.x + 0.5;
        var c = vec3<f32>(1.0, 1.0, 0.0);
        if (k < 3.0) { c = vec3<f32>(0.0, 0.0, 1.0); }
        if (k < 2.0) { c = vec3<f32>(0.0, 1.0, 0.0); }
        if (k < 1.0) { c = vec3<f32>(1.0, 0.0, 0.0); }
        out.base_color = vec3<f32>(0.0);
        out.emissive = c;
        return out;
    }`;

const XS = [-2, -0.7, 0.7, 2];
const SIZE = 128;
const column = (x) => Math.round(SIZE / 2 + x * SIZE / (2 * 6 * Math.tan(Math.PI / 6)));

function seen(img) {
    return XS.map((x) => {
        const i = ((SIZE / 2) * img.width + column(x)) * 4;
        const r = img.data[i], g = img.data[i + 1], b = img.data[i + 2];
        const on = (v) => v > 128, off = (v) => v < 64;
        if (on(r) && off(g) && off(b)) return 'red';
        if (off(r) && on(g) && off(b)) return 'green';
        if (off(r) && off(g) && on(b)) return 'blue';
        if (on(r) && on(g) && off(b)) return 'yellow';
        if (on(r) && off(g) && on(b)) return 'wall';
        if (off(r) && off(g) && off(b)) return 'nothing';
        return `(${r},${g},${b})`;
    }).join(' ');
}

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else if (!bro.kernel.available || !bro.kernel.gpu) {
    dropScene(probe);
    skipTest('no GPU device for bro.kernel');
} else {
    dropScene(probe);
    const xs = bro.kernel.createBuffer(new Float32Array(XS));
    const spheres = bro.kernel.createBuffer(new Float32Array(XS.flatMap((x) => [x, 0, 0, 0.9])));
    const rows = new Float32Array(16 * XS.length);
    for (let k = 0; k < XS.length; k++) rows.set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1], 16 * k);
    const all = 'red green blue yellow', behind = 'wall wall blue yellow';

    for (const msaa of [1, 4]) {
        const s = freshScene(SIZE), sc = s.scene;
        sc.setMSAA(msaa);
        const node = sc.createInstancedMesh({ mesh: Mesh.box(), color: [1, 1, 1, 1], instances: rows });
        node.setShader({ module: hook });
        node.setShaderBuffer('xs', xs);
        node.setCullBounds([-3, -1, -1], [3, 1, 1]);
        node.setInstanceCullSpheres(spheres);
        // A wall before the two on the left, wide enough to hide their spheres.
        const wall = sc.createMesh({ mesh: 'box', halfW: 1.35, halfH: 2, halfD: 0.05, color: [0, 0, 0],
                                     emissive: 1, emissiveColor: [1, 0, 1], x: -1.25, y: 0, z: 2 });
        let got = seen(sc.captureFrame());
        assert(got === behind, `msaa ${msaa}: the first frame shows what the wall leaves in sight: ${got}`);
        for (let k = 0; k < 3; k++) got = seen(sc.captureFrame());
        assert(got === behind, `msaa ${msaa}: and so do the frames after it: ${got}`);
        wall.setPosition(-1.25, 50, 2);
        got = seen(sc.captureFrame());
        assert(got === all, `msaa ${msaa}: the frame the wall is gone, every instance shows: ${got}`);
        got = seen(sc.captureFrame());
        assert(got === all, `msaa ${msaa}: and the frame after: ${got}`);
        wall.setPosition(-1.25, 0, 2);
        for (let k = 0; k < 4; k++) got = seen(sc.captureFrame());
        assert(got === behind, `msaa ${msaa}: behind the wall again: ${got}`);
        // The camera turned by the wall's side: what was hidden shows at once.
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 1.2], target: [0, 0, 0] });
        const img = sc.captureFrame();
        const mid = ((SIZE / 2) * img.width + Math.round(SIZE * 0.2)) * 4;
        assert(img.data[mid + 1] > 128 && img.data[mid] < 64 && img.data[mid + 2] < 64,
               `msaa ${msaa}: past the wall, the green one fills its place the frame the eye is there: ` +
               `${img.data[mid]},${img.data[mid + 1]},${img.data[mid + 2]}`);
        dropScene(s);
    }
}

// Instanced draws whose instances the GPU decides, by their pixels. A vertex
// hook places instance k at x = xs[k] (a KernelBuffer) from identity rows
// and colours it by its instance_index (red, green, blue, yellow), so what
// draws, and as which instance, shows.
//   - setInstanceCountBuffer: the count a buffer holds draws instances
//     0..n-1, clamped to the rows; changing the buffer changes the next frame;
//   - setInstanceCullSpheres: an instance whose sphere is outside the view
//     does not draw, the others draw as themselves (a culled view's
//     instances are compacted; the hook's instance_index is still each
//     one's own), with or without a count;
//   - setCullBounds: the node is culled by its box, not by its rows;
//   - the culled instances cast shadows as the uncut ones do.

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

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
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
// ~18.5 px a unit at z = 0 (60° fov, 6 units away, 128 px).
const column = (x) => Math.round(SIZE / 2 + x * SIZE / (2 * 6 * Math.tan(Math.PI / 6)));

// What each instance's place shows: the colour's name.
function seen(img) {
    return XS.map((x) => {
        const [r, g, b] = px(img, column(x), SIZE / 2);
        const on = (v) => v > 128, off = (v) => v < 64;
        if (on(r) && off(g) && off(b)) return 'red';
        if (off(r) && on(g) && off(b)) return 'green';
        if (off(r) && off(g) && on(b)) return 'blue';
        if (on(r) && on(g) && off(b)) return 'yellow';
        if (off(r) && off(g) && off(b)) return 'nothing';
        return `(${r},${g},${b})`;
    }).join(' ');
}

const words = (u32) => new Float32Array(new Uint32Array(u32).buffer);

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else if (!bro.kernel.available || !bro.kernel.gpu) {
    dropScene(probe);
    skipTest('no GPU device for bro.kernel');
} else {
    dropScene(probe);
    const xs = bro.kernel.createBuffer(new Float32Array(XS));
    const rows = new Float32Array(16 * XS.length);
    for (let k = 0; k < XS.length; k++) rows.set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1], 16 * k);
    const make = (sc) => {
        const node = sc.createInstancedMesh({ mesh: Mesh.box(), color: [1, 1, 1, 1], instances: rows });
        node.setShader({ module: hook });
        node.setShaderBuffer('xs', xs);
        // The hook moves the instances off their rows: the box they stay in.
        node.setCullBounds([-3, -1, -1], [3, 1, 1]);
        return node;
    };
    const all = 'red green blue yellow';

    // Every row, as itself.
    {
        const s = freshScene(SIZE);
        make(s.scene);
        const got = seen(s.scene.captureFrame());
        assert(got === all, `every instance draws as itself: ${got}`);
        dropScene(s);
    }

    // A GPU count: the first n, clamped to the rows.
    {
        const s = freshScene(SIZE);
        const node = make(s.scene);
        const counts = bro.kernel.createBuffer(words([0, 2]));
        node.setInstanceCountBuffer(counts, 1);
        let got = seen(s.scene.captureFrame());
        assert(got === 'red green nothing nothing', `a count of 2 draws the first two: ${got}`);
        counts.write(words([0, 99]));
        got = seen(s.scene.captureFrame());
        assert(got === all, `a count past the rows draws every row: ${got}`);
        counts.write(words([0, 0]));
        got = seen(s.scene.captureFrame());
        assert(got === 'nothing nothing nothing nothing', `a count of 0 draws none: ${got}`);
        let threw = null;
        try { node.setInstanceCountBuffer(counts, 2); } catch (e) { threw = e; }
        assert(threw instanceof RangeError, 'a word outside the buffer is refused: ' + (threw && threw.message));
        threw = null;
        try { node.setInstanceCountBuffer(new Float32Array(4), 0); } catch (e) { threw = e; }
        assert(threw instanceof TypeError, 'a typed array is not a KernelBuffer: ' + (threw && threw.message));
        node.setInstanceCountBuffer(null, 0);
        got = seen(s.scene.captureFrame());
        assert(got === all, `without a count buffer every row draws again: ${got}`);
        dropScene(s);
    }

    // Spheres: green's says it is far off to the side, so the camera's view
    // drops it; the rest draw as themselves though they are compacted.
    {
        const s = freshScene(SIZE);
        const node = make(s.scene);
        const spheres = new Float32Array(16);
        XS.forEach((x, k) => spheres.set([k === 1 ? 100 : x, 0, 0, 0.9], 4 * k));
        node.setInstanceCullSpheres(bro.kernel.createBuffer(spheres));
        let got = seen(s.scene.captureFrame());
        assert(got === 'red nothing blue yellow', `an instance whose sphere is out of view is culled: ${got}`);
        const counts = bro.kernel.createBuffer(words([3]));
        node.setInstanceCountBuffer(counts, 0);
        got = seen(s.scene.captureFrame());
        assert(got === 'red nothing blue nothing', `culled within a count: ${got}`);
        node.setInstanceCullSpheres(null);
        got = seen(s.scene.captureFrame());
        assert(got === 'red green blue nothing', `without spheres the count alone: ${got}`);
        dropScene(s);
    }

    // Shadows: the culled caster's shadow is the uncut one's. A low sun
    // throws the boxes' shadows across a white floor beside them; the camera
    // looks down.
    {
        const shadowOf = (cull, cast = true) => {
            const s = freshScene(SIZE);
            const sc = s.scene;
            sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 8, 0.01], target: [0, 0, 0] });
            sc.setAmbient({ color: [0.2, 0.2, 0.2] });
            sc.createLight({ type: 'directional', direction: [0, -1, 0.8], intensity: 3, castShadow: true });
            sc.createMesh({ mesh: Mesh.plane(12, 12, 1, 1), color: [1, 1, 1], roughness: 1, castsShadow: false })
                .position = [0, -1.5, 0];
            const node = make(sc);
            node.castsShadow = cast;
            if (cull) {
                const spheres = new Float32Array(16);
                XS.forEach((x, k) => spheres.set([x, 0, 0, 0.9], 4 * k));
                node.setInstanceCullSpheres(bro.kernel.createBuffer(spheres));
            }
            for (let i = 0; i < 3; i++) sc.captureFrame();
            const img = sc.captureFrame();
            dropScene(s);
            return img;
        };
        const differ = (a, b) => {
            let n = 0;
            for (let i = 0; i < a.data.length; i += 4)
                if (Math.abs(a.data[i] - b.data[i]) > 2 || Math.abs(a.data[i + 1] - b.data[i + 1]) > 2) n++;
            return n;
        };
        const lit = shadowOf(false, false), a = shadowOf(false), b = shadowOf(true);
        const shade = differ(lit, a);
        assert(shade > 200, `the boxes cast shadows on the floor (${shade} px darker)`);
        const d = differ(a, b);
        assert(d <= 4, `culled casters cast as the uncut ones do (${d} px differ)`);
    }
}

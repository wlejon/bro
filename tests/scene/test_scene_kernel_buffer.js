// A scene drawing straight from kernel output: a custom shader's
// `buffer float u_heights[];` reads a KernelBuffer a GPU kernel filled, in
// place. The oracle is the same grid with the kernel's CPU heights baked into
// its positions: both must render the same pixels.
//   - an unbound buffer reads as length 0 (the grid stays flat);
//   - a buffer filled by dispatch and bound displaces the grid exactly as
//     the baked heights do;
//   - a second dispatch into the same buffer is seen by the next frame;
//   - a value that is not a KernelBuffer, and a buffer of a type a buffer
//     cannot hold, are refused.

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

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// An N x N grid over [-1, 1]^2 in xz, vertex i = z * N + x (the kernel's
// cell order), heights `h` (or flat).
function grid(N, h) {
    const positions = new Float32Array(N * N * 3), normals = new Float32Array(N * N * 3);
    for (let z = 0; z < N; z++)
        for (let x = 0; x < N; x++) {
            const i = z * N + x;
            positions.set([-1 + 2 * x / (N - 1), h ? h[i] : 0, -1 + 2 * z / (N - 1)], i * 3);
            normals.set([0, 1, 0], i * 3);
        }
    const indices = new Uint32Array((N - 1) * (N - 1) * 6);
    let k = 0;
    for (let z = 0; z < N - 1; z++)
        for (let x = 0; x < N - 1; x++) {
            const a = z * N + x, b = a + 1, c = a + N, d = c + 1;
            indices.set([a, c, b, b, c, d], k);
            k += 6;
        }
    return { positions, normals, indices };
}

// The height as a colour, so the displaced surface is what the pixels show.
const fragment = `in float v_h;
    void userFragment(inout vec3 baseColor, inout vec3 normal,
                      inout float metallic, inout float roughness,
                      inout vec3 emissive, inout float alpha) {
        baseColor = vec3(0.0);
        emissive = vec3(0.5 + v_h, 0.5 - v_h, 0.25);
    }`;
const baked = `out float v_h;
    void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
        v_h = pos.y;
    }`;
const fromBuffer = `buffer float u_heights[];
    out float v_h;
    void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
        if (uint(gl_VertexIndex) < u_heights_length) pos.y += u_heights[gl_VertexIndex];
        v_h = pos.y;
    }`;

function frameDiff(a, b) {
    let worst = 0, count = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        const d = Math.max(Math.abs(a.data[i] - b.data[i]), Math.abs(a.data[i + 1] - b.data[i + 1]),
                           Math.abs(a.data[i + 2] - b.data[i + 2]));
        worst = Math.max(worst, d);
        if (d > 2) count++;
    }
    return { worst, count };
}

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else if (!bro.kernel.available || !bro.kernel.gpu) {
    dropScene(probe);
    skipTest('no GPU device for bro.kernel');
} else {
    dropScene(probe);
    const S = 128, N = 48;
    const heights = bro.kernel.compile(`
        @kernel @grid(2)
        fn k_heights(g: grid<2>, out: write_store<f32, 2>, k: f32) {
            let x = g.x() as f32;
            let z = g.y() as f32;
            out[g.x(), g.y()] = 0.35 * sin(x * k) * cos(z * k * 0.7);
        }
    `, { file: 'heights.bk' }).kernel('k_heights');

    const render = (vertex, mesh, bind) => {
        const s = freshScene(S);
        const node = s.scene.createMesh(Object.assign({ color: [1, 1, 1] }, mesh));
        node.setShader({ vertex, fragment });
        if (bind) bind(node);
        const img = s.scene.captureFrame();
        return { s, node, img };
    };
    const cpuHeights = (k) => {
        const h = new Float32Array(N * N);
        heights.run([N, N], h, k);
        return h;
    };

    // Unbound: length 0, the grid stays flat.
    {
        const flat = render(baked, grid(N, null));
        const unbound = render(fromBuffer, grid(N, null));
        const d = frameDiff(flat.img, unbound.img);
        assert(d.count === 0, `an unbound buffer reads as length 0 (worst ${d.worst}, ${d.count} px differ)`);
        dropScene(flat.s);
        dropScene(unbound.s);
    }

    // Filled on the GPU and bound: the same pixels as the baked heights.
    const buf = bro.kernel.createBuffer(N * N * 4);
    heights.dispatch([N, N], buf, 0.31);
    const want = render(baked, grid(N, cpuHeights(0.31)));
    const got = render(fromBuffer, grid(N, null), (node) => node.setShaderBuffer('u_heights', buf));
    const flatImg = render(baked, grid(N, null));
    const vsFlat = frameDiff(want.img, flatImg.img);
    assert(vsFlat.count > 500, `the baked heights change the picture (${vsFlat.count} px)`);
    dropScene(flatImg.s);
    let d = frameDiff(want.img, got.img);
    assert(d.count <= 4, `the kernel's buffer displaces the grid as the baked heights do (worst ${d.worst}, ${d.count} px differ)`);

    // A second dispatch into the bound buffer is seen by the next frame.
    heights.dispatch([N, N], buf, 0.17);
    const want2 = render(baked, grid(N, cpuHeights(0.17)));
    d = frameDiff(want2.img, got.s.scene.captureFrame());
    assert(d.count <= 4, `a dispatch into the bound buffer reaches the next frame (worst ${d.worst}, ${d.count} px differ)`);
    dropScene(want.s);
    dropScene(want2.s);

    // Unbinding goes back to length 0.
    got.node.setShaderBuffer('u_heights', null);
    const flatAgain = render(baked, grid(N, null));
    d = frameDiff(flatAgain.img, got.s.scene.captureFrame());
    assert(d.count === 0, `unbinding reads as length 0 again (worst ${d.worst}, ${d.count} px differ)`);
    dropScene(flatAgain.s);

    // The errors a script gets.
    let threw = null;
    try { got.node.setShaderBuffer('u_heights', new Float32Array(N * N)); } catch (e) { threw = e; }
    assert(threw instanceof TypeError && /KernelBuffer/.test(threw.message),
           'a typed array is not a KernelBuffer: ' + (threw && threw.message));
    threw = null;
    try {
        got.node.setShader({ vertex: `buffer bool u_flags[];
            void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {}` });
    } catch (e) { threw = e; }
    assert(threw && /buffer holds/.test(threw.message), 'a buffer of bools is refused: ' + (threw && threw.message));
    dropScene(got.s);
}

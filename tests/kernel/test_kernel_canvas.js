// canvas.getContext('kernel'): a canvas that shows a KernelBuffer's pixels,
// copied on the GPU after the kernels that wrote them (docs/kernel-api.js
// section 8). A kernel fills RGBA8 and RGBA32F buffers; the composited page
// shows them, rows top to bottom; the errors a script gets for bad calls.
assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else if (!bro.kernel.gpu) {
    missingGpuContext('kernel');
} else {
    const W = 48, H = 32;
    const root = document.getElementById('root');
    root.innerHTML =
        `<canvas id="a" width="${W}" height="${H}" style="position:absolute;left:0;top:0;width:${W}px;height:${H}px"></canvas>` +
        `<canvas id="b" width="${W}" height="${H}" style="position:absolute;left:60px;top:0;width:${W}px;height:${H}px"></canvas>`;
    flush();
    const a = document.getElementById('a');
    const b = document.getElementById('b');

    const mod = bro.kernel.compile(`
        @kernel @grid(2)
        fn k_rgba8(g: grid<2>, out: write_store<u32, 2>, w: i32, h: i32) {
            let r = (g.x() * 255 / (w - 1)) as u32;
            let gg = (g.y() * 255 / (h - 1)) as u32;
            out[g.x(), g.y()] = r | (gg << 8u) | (64u << 16u) | (255u << 24u);
        }
        @kernel @grid(2)
        fn k_rgba32f(g: grid<2>, out: write_store<vec4<f32>, 2>, w: i32) {
            let t = (g.x() as f32) / ((w - 1) as f32);
            out[g.x(), g.y()] = vec4<f32>(1.0 - t, 0.25, t, 1.0);
        }
    `, { file: 'kernel_canvas.bk' });

    const ctx = a.getContext('kernel');
    assert(ctx && typeof ctx.present === 'function', "getContext('kernel') returns a context");
    assert(a.getContext('kernel') === ctx, 'the same context comes back');
    assert(ctx.canvas === a, 'ctx.canvas is the canvas');
    assert(a.getContext('2d') === null && a.getContext('webgl2') === null, 'a kernel canvas has no other context');

    // RGBA8: red across, green down, row 0 at the top.
    const px8 = bro.kernel.createBuffer(W * H * 4);
    mod.kernel('k_rgba8').dispatch([W, H], px8, W, H);
    ctx.present(px8);
    let p = getPixels(0, 0, W, H).data;
    const at = (x, y) => [p[(y * W + x) * 4], p[(y * W + x) * 4 + 1], p[(y * W + x) * 4 + 2]];
    let c = at(0, 0);
    assert(c[0] <= 2 && c[1] <= 2 && Math.abs(c[2] - 64) <= 2, 'rgba8 top-left is (0, 0, 64): ' + c);
    c = at(W - 1, H - 1);
    assert(c[0] >= 253 && c[1] >= 253, 'rgba8 bottom-right is (255, 255, .): ' + c);
    c = at(W - 1, 0);
    assert(c[0] >= 253 && c[1] <= 2, 'rgba8 top-right is red: ' + c);

    // A later dispatch into the same buffer shows after the next present.
    const ctxB = b.getContext('kernel');
    const pxF = bro.kernel.createBuffer(16 + W * H * 16);
    mod.kernel('k_rgba32f').dispatch([W, H], pxF, W);
    // byteOffset: the texels start past a 16-byte header the kernel did not
    // write; showing from 0 would shift every row by one texel.
    const pxF2 = bro.kernel.createBuffer(16 + W * H * 16);
    const host = new Float32Array(W * H * 4);
    pxF.read(host);
    pxF2.write(host, 16);
    ctxB.present(pxF2, { format: 'rgba32f', byteOffset: 16 });
    p = getPixels(60, 0, W, H).data;
    c = at(0, 5);
    assert(c[0] >= 250 && Math.abs(c[1] - 64) <= 2 && c[2] <= 4, 'rgba32f left column is (1, .25, 0): ' + c);
    c = at(W - 1, 5);
    assert(c[0] <= 4 && c[2] >= 250, 'rgba32f right column is blue: ' + c);

    // A canvas resized shows its new size.
    a.width = 16; a.height = 16;
    const small = bro.kernel.createBuffer(16 * 16 * 4);
    mod.kernel('k_rgba8').dispatch([16, 16], small, 16, 16);
    ctx.present(small);
    p = getPixels(0, 0, W, H).data;
    c = at(W - 1, H - 1);
    assert(c[0] >= 253 && c[1] >= 253, 'a 16x16 buffer fills the resized canvas: ' + c);

    // Errors.
    const throwsAs = (fn, type, re, label) => {
        let err = null;
        try { fn(); } catch (e) { err = e; }
        assert(err instanceof type && re.test(err.message), label + ' (got ' + err + ')');
    };
    throwsAs(() => ctx.present(new Uint8Array(16 * 16 * 4)), TypeError, /not a KernelBuffer/, 'a typed array is refused');
    throwsAs(() => ctx.present(small, { format: 'bgra8' }), TypeError, /format/, 'an unknown format is refused');
    throwsAs(() => ctx.present(bro.kernel.createBuffer(64)), RangeError, /texels/, 'a buffer too small is refused');
    throwsAs(() => ctx.present(small, { byteOffset: 4 }), RangeError, /texels/, 'texels past the end are refused');
    throwsAs(() => ctx.present(small, { byteOffset: -4 }), RangeError, /byteOffset/, 'a negative byteOffset is refused');
    throwsAs(() => ctx.present(pxF2, { format: 'rgba32f', byteOffset: 4 }), RangeError, /multiple/,
             'a byteOffset inside a texel is refused');
    let inJob = null;
    bro.kernel.background(() => {
        try { ctx.present(small); } catch (e) { inJob = e; }
    });
    assert(inJob instanceof TypeError, 'present inside bro.kernel.background throws: ' + inJob);

    // A 2d canvas has no kernel context.
    const other = document.createElement('canvas');
    root.appendChild(other);
    other.getContext('2d');
    assert(other.getContext('kernel') === null, "a 2d canvas's getContext('kernel') is null");

    // Removed from the page, its canvas goes with it.
    root.innerHTML = '';
    flush();
}

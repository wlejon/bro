// bro.vision results carry ImageBitmaps again (src/bronze_host/host_vision*.cpp).
//
// The old QuickJS binding returned `image` — and BiRefNet's `matte` — as
// engine-minted ImageBitmaps; the bronze port moved the models into
// brovisionml, which cannot mint one, so they came back as typed arrays only.
// bro re-drives the ops and rasterizes the planes back into bitmaps WITHOUT
// taking the typed-array planes away.
//
// WEIGHTS-FREE. Every model op needs a checkpoint (brovisionml's loaders
// refuse a directory without one), so the rasterizer family — the part that
// is bro's — is driven through `__host.visionProbe`, whose synthetic compute
// writes the ramp plane[i] = i / (n - 1) and whose `kind` picks the same
// rasterizer a model op would use. Each bitmap is drawn onto a canvas and its
// pixels read back: the SHAPE and the COLOUR MAPPING are the subject.
//
// Also covered weights-free: bro's re-driven ops refuse a wrong receiver with
// a TypeError, the same as the sibling's.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

function assertBitmap(v, w, h, what) {
    assert(v instanceof ImageBitmap, what + ' is an ImageBitmap');
    assert(v.width === w, what + '.width is ' + w + ', got ' + v.width);
    assert(v.height === h, what + '.height is ' + h + ', got ' + v.height);
}

// The bitmap's pixels, through the 2D canvas it would be drawn with.
function pixelsOf(bmp) {
    const c = document.createElement('canvas');
    c.width = bmp.width;
    c.height = bmp.height;
    const ctx = c.getContext('2d');
    ctx.clearRect(0, 0, c.width, c.height);
    ctx.drawImage(bmp, 0, 0);
    return ctx.getImageData(0, 0, c.width, c.height).data;
}

function px(data, w, x, y) {
    const i = (y * w + x) * 4;
    return [data[i], data[i + 1], data[i + 2], data[i + 3]];
}

function near(a, b, tol) { return Math.abs(a - b) <= tol; }

function assertPx(got, want, tol, what) {
    const ok = got.every((v, i) => near(v, want[i], tol));
    assert(ok, what + ': want [' + want.join(',') + '] got [' + got.join(',') + ']');
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.vision !== undefined && bro.vision !== null, 'bro.vision namespace exists');

if (bro.vision.available === false || typeof __host.visionProbe !== 'function') {
    console.log('bro.vision is compiled out; nothing to rasterize');
} else {
    assert(typeof ImageBitmap === 'function', 'ImageBitmap constructor is reachable');
    const probe = __host.visionProbe;
    const W = 12, H = 9, N = W * H;

    // ── gray, min/max normalized (depth's rasterizer) ───────────────────────
    {
        const r = probe(W, H);
        assert(r.plane instanceof Float32Array && r.plane.length === N, 'the plane stays a Float32Array');
        assert(r.min === 0 && r.max === 1, 'min/max report the observed range: ' + r.min + '..' + r.max);
        assertBitmap(r.image, W, H, 'gray image');
        const d = pixelsOf(r.image);
        assertPx(px(d, W, 0, 0), [0, 0, 0, 255], 1, 'gray first pixel is black');
        assertPx(px(d, W, W - 1, H - 1), [255, 255, 255, 255], 1, 'gray last pixel is white');

        const inv = probe(W, H, { invert: true });
        const di = pixelsOf(inv.image);
        assertPx(px(di, W, 0, 0), [255, 255, 255, 255], 1, 'inverted first pixel is white');
        assertPx(px(di, W, W - 1, H - 1), [0, 0, 0, 255], 1, 'inverted last pixel is black');
    }

    // ── gray, unit scale (HED / lineart / matte) ────────────────────────────
    {
        const r = probe(W, H, { kind: 'unit' });
        assertBitmap(r.image, W, H, 'unit image');
        const d = pixelsOf(r.image);
        const mid = Math.floor(N / 2);
        const want = Math.round(mid / (N - 1) * 255);
        assertPx(px(d, W, mid % W, Math.floor(mid / W)), [want, want, want, 255], 1,
                 'unit gray maps v to v*255');
    }

    // ── RGBA straight through, RGB forced opaque ────────────────────────────
    {
        const r = probe(W, H, { kind: 'rgba' });
        assertBitmap(r.image, W, H, 'rgba image');
        const a = px(pixelsOf(r.image), W, W - 1, H - 1);
        assert(near(a[3], 128, 1), 'rgba keeps the source alpha, got ' + a[3]);

        const g = probe(W, H, { kind: 'rgb' });
        assertBitmap(g.image, W, H, 'rgb image');
        assertPx(px(pixelsOf(g.image), W, W - 1, H - 1), [255, 255, 255, 255], 1,
                 'rgb is opaque');
    }

    // ── a 0/1 mask as the dodger-blue overlay (SAM) ─────────────────────────
    {
        const r = probe(W, H, { kind: 'mask' });
        assertBitmap(r.image, W, H, 'mask image');
        const d = pixelsOf(r.image);
        assertPx(px(d, W, 0, 0), [0, 0, 0, 0], 0, 'mask background is transparent');
        assertPx(px(d, W, W - 1, H - 1), [30, 144, 255, 255], 1, 'mask foreground is the overlay colour');
    }

    // ── planar unit normals through (n+1)/2 ─────────────────────────────────
    {
        const r = probe(W, H, { kind: 'normals' });
        assertBitmap(r.image, W, H, 'normals image');
        assertPx(px(pixelsOf(r.image), W, 3, 3), [128, 128, 255, 255], 1, '+Z is (128,128,255)');
    }

    // ── MLSD segments as white strokes on transparent black ─────────────────
    {
        const r = probe(W, H, { kind: 'segments' });
        assertBitmap(r.image, W, H, 'segments image');
        const d = pixelsOf(r.image);
        assertPx(px(d, W, 0, 0), [255, 255, 255, 255], 0, 'the stroke starts at its first endpoint');
        assertPx(px(d, W, W - 1, H - 1), [255, 255, 255, 255], 0, 'the stroke ends at its second');
        assertPx(px(d, W, W - 1, 0), [0, 0, 0, 0], 0, 'off the stroke is transparent');
    }

    // ── async results carry the same bitmap ─────────────────────────────────
    {
        let got = null;
        probe(W, H, { kind: 'mask', onDone(r) { got = r; } });
        const t0 = Date.now();
        while (got === null && Date.now() - t0 < 5000) { advanceTime(16); wallSleep(2); }
        assert(got !== null, 'async probe settled');
        assertBitmap(got.image, W, H, 'async mask image');
    }

    // ── bro's re-driven ops check their receiver like the sibling's ─────────
    {
        const ops = [
            ['DepthEstimator', 'estimate'], ['NormalEstimator', 'estimate'],
            ['Hed', 'detect'], ['Lineart', 'detect'], ['Mlsd', 'detect'],
            ['Openpose', 'detect'], ['Segformer', 'detect'],
            ['Birefnet', 'removeBackground'], ['Sam', 'segment'],
            ['StyleGAN3', 'generate'],
        ];
        for (const [cls, m] of ops) {
            const C = globalThis[cls];
            assert(typeof C === 'function' && typeof C.prototype[m] === 'function',
                   cls + '.prototype.' + m + ' is reachable');
            const e = expectThrows(() => C.prototype[m].call({}, { width: 1, height: 1, data: new Uint8Array(4) }),
                                   cls + '.prototype.' + m + ' on a plain object');
            assert(e instanceof TypeError, cls + '.' + m + ' wrong receiver is a TypeError: ' + e);
        }
    }

    console.log('bro.vision ImageBitmap results OK (weights-free)');
}

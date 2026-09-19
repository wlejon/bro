// bro.vision results carry ImageBitmaps again (src/bronze_host/host_vision*.cpp).
//
// The old QuickJS binding returned `image` — and BiRefNet's `matte` — as
// engine-minted ImageBitmaps; the bronze port moved the models into
// brovisionml, which cannot mint one, so they came back as typed arrays only.
// bro now re-drives the ops and puts the bitmaps back WITHOUT taking the
// typed-array planes away.
//
// WEIGHTS-FREE. Every loader here is handed '.', a directory that exists (so
// the loader does not throw) and holds no checkpoint (so the model reports
// loaded=false). Each op then runs its documented probe answer — a flat
// plane, an empty mask list — which is exactly what a bitmap test needs: the
// pixels are boring, the SHAPE is the subject. Nothing downloads.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

// An RGBA image object in the { width, height, data } form every vision op
// accepts. Small, because the placeholder planes are allocated at this size.
function makeImage(w, h) {
    const data = new Uint8Array(w * h * 4);
    for (let i = 0; i < w * h; i++) {
        data[4 * i + 0] = (i * 7) & 255;
        data[4 * i + 1] = (i * 13) & 255;
        data[4 * i + 2] = (i * 29) & 255;
        data[4 * i + 3] = 255;
    }
    return { width: w, height: h, data };
}

function assertBitmap(v, w, h, what) {
    assert(v !== undefined, what + ' is present');
    assert(v !== null, what + ' is not null');
    assert(typeof v === 'object', what + ' is an object');
    assert(v instanceof ImageBitmap, what + ' is an ImageBitmap');
    assert(v.width === w, what + '.width is ' + w + ', got ' + v.width);
    assert(v.height === h, what + '.height is ' + h + ', got ' + v.height);
}

function assertTypedArray(v, ctor, len, what) {
    assert(v instanceof ctor, what + ' is a ' + ctor.name);
    assert(v.length === len, what + '.length is ' + len + ', got ' + v.length);
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.vision !== undefined && bro.vision !== null, 'bro.vision namespace exists');

if (bro.vision.available === false) {
    console.log('bro.vision is the unavailable stub; nothing to wrap');
} else {
    assert(typeof ImageBitmap === 'function', 'ImageBitmap constructor is reachable');

    const W = 12, H = 9;
    const img = makeImage(W, H);
    const dir = '.';   // exists, holds no weights

    // ── depth: { width, height, depth, gray, min, max, image } ──────────────
    {
        const depth = bro.vision.loadDepth(dir);
        const r = depth.estimate(img);
        assert(r.width === W && r.height === H, 'depth result keeps the input size');
        assertTypedArray(r.depth, Float32Array, W * H, 'depth.depth');
        assertTypedArray(r.gray, Uint8Array, W * H, 'depth.gray');
        assert(typeof r.min === 'number' && typeof r.max === 'number',
               'depth reports min/max');
        assertBitmap(r.image, W, H, 'depth.image');

        // invert flips the gray plane, and the bitmap with it.
        const inv = depth.estimate(img, { invert: true });
        assertBitmap(inv.image, W, H, 'depth.image (inverted)');
        assertTypedArray(inv.gray, Uint8Array, W * H, 'depth.gray (inverted)');
    }

    // ── normals: { width, height, normals, normal, image } ──────────────────
    {
        const normal = bro.vision.loadNormal(dir);
        const r = normal.estimate(img);
        assert(r.width === W && r.height === H, 'normal result keeps the input size');
        assertTypedArray(r.normals, Float32Array, W * H * 3, 'normal.normals');
        assert(r.normal === r.normals, 'normal.normal aliases normal.normals');
        assertBitmap(r.image, W, H, 'normal.image');
    }

    // ── HED / Lineart: the unit-scalar plane under both spellings + bitmap ──
    {
        const hed = bro.vision.loadHed(dir);
        const r = hed.detect(img);
        assertTypedArray(r.edge, Float32Array, W * H, 'hed.edge');
        assertTypedArray(r.edges, Uint8Array, W * H, 'hed.edges');
        assertBitmap(r.image, W, H, 'hed.image');
        // estimate() is the port's name for the same body; it wraps too.
        assertBitmap(hed.estimate(img).image, W, H, 'hed.estimate().image');

        const lineart = bro.vision.loadLineart(dir);
        const l = lineart.detect(img);
        assertTypedArray(l.line, Float32Array, W * H, 'lineart.line');
        assertTypedArray(l.lines, Uint8Array, W * H, 'lineart.lines');
        assertBitmap(l.image, W, H, 'lineart.image');
    }

    // ── MLSD: segments array under both spellings + a stroke overlay ────────
    {
        const mlsd = bro.vision.loadMlsd(dir);
        const r = mlsd.detect(img);
        assert(Array.isArray(r.segments), 'mlsd.segments is an array');
        assert(r.lines === r.segments, 'mlsd.lines aliases mlsd.segments');
        assertBitmap(r.image, W, H, 'mlsd.image');
    }

    // ── OpenPose: bodies under both spellings + the skeleton canvas ─────────
    {
        const pose = bro.vision.loadOpenpose(dir);
        const r = pose.detect(img);
        assert(Array.isArray(r.bodies), 'openpose.bodies is an array');
        assert(r.poses === r.bodies, 'openpose.poses aliases openpose.bodies');
        assert(r.image === null || r.image instanceof ImageBitmap,
               'openpose.image is an ImageBitmap or null for an empty canvas');
    }

    // ── SegFormer: class ids as bytes and as Int32 + the palette bitmap ─────
    {
        const seg = bro.vision.loadSegformer(dir);
        const r = seg.detect(img);
        assertTypedArray(r.classes, Uint8Array, W * H, 'segformer.classes');
        assertTypedArray(r.segments, Int32Array, W * H, 'segformer.segments');
        assertBitmap(r.image, W, H, 'segformer.image');
    }

    // ── BiRefNet: TWO bitmaps, and the planes still typed arrays ────────────
    {
        const br = bro.vision.loadBirefnet(dir);
        const r = br.removeBackground(img);
        assert(r.width === W && r.height === H, 'removeBackground keeps the input size');
        assertTypedArray(r.alpha, Float32Array, W * H, 'birefnet.alpha');
        assertTypedArray(r.mask, Uint8Array, W * H, 'birefnet.mask (the matte bytes)');
        assertTypedArray(r.data, Uint8Array, W * H * 4, 'birefnet.data (the cutout)');
        assertBitmap(r.matte, W, H, 'birefnet.matte');
        assertBitmap(r.image, W, H, 'birefnet.image');
        // The cutout's alpha is the matte: with no weights the matte is 0, so
        // every cutout pixel is fully transparent while RGB is untouched.
        assert(r.data[3] === r.mask[0], 'cutout alpha comes from the matte');
        assert(r.data[0] === img.data[0], 'cutout keeps the source RGB');
    }

    // ── SAM: the mask list shape, and no crash without an embedding ─────────
    {
        const sam = bro.vision.loadSam(dir);
        sam.setImage(img);
        assert(sam.hasImage === true, 'setImage records the frame');
        const r = sam.segment({ points: [[3, 3]] });
        assert(typeof r.num === 'number', 'sam.segment reports num');
        assert(Array.isArray(r.masks), 'sam.segment masks is an array');
        for (const m of r.masks) {
            assertBitmap(m.image, r.width, r.height, 'sam mask image');
        }
        const e = sam.segmentEverything(img);
        assert(Array.isArray(e.masks), 'segmentEverything masks is an array');
        assert(e.width === W && e.height === H, 'segmentEverything keeps the input size');
        for (const m of e.masks) {
            assertBitmap(m.image, m.width, m.height, 'segmentEverything mask image');
        }
        // A prompt-less segment is still a TypeError, weights or not.
        const err = expectThrows(() => sam.segment({}), 'segment with no prompt');
        assert(err instanceof TypeError, 'prompt-less segment throws TypeError');
    }

    // ── StyleGAN3: generate / synthesize carry the rendered bitmap ──────────
    {
        const gan = bro.vision.loadStyleGAN3(dir, { resolution: 256 });
        const g = gan.generate({ seed: 7 });
        assert(g.width === 256 && g.height === 256, 'generate renders at 256');
        assertTypedArray(g.data, Uint8Array, 256 * 256 * g.channels, 'stylegan3.data');
        assertBitmap(g.image, 256, 256, 'stylegan3 generate image');

        const s = gan.synthesize(new Float32Array(gan.numWs * gan.wDim));
        assertBitmap(s.image, 256, 256, 'stylegan3 synthesize image');
    }

    console.log('bro.vision ImageBitmap results OK (weights-free)');
}

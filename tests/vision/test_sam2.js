// SAM 2.1 end to end: bro.vision.loadSam2 -> Sam2.segment on one image, and
// Sam2Tracker following one object through a synthetic burst (click on frame
// 0, propagate, a correction click on frame 2, propagate again), with the
// async form (onDone, delivered by the frame pump) beside the sync one.
//
// The scene is drawn here: a "bird" (body ellipse + head disc) on a branch
// over a gradient, moving right and up across four 640x480 frames, so every
// mask is checked against the shape it should cover. Parity with the
// reference implementation is brovisionml's test (tests/test_sam2.cpp); this
// checks the binding, the job machine and the tracking contract. Masks are
// scored by birdIoU below: precision on the bird and recall of its body.
//
// Weights-gated: SAM2_DIR, else bro.models.find('sam2.1-hiera-base-plus')
// (BRO_MODELS=D:/models, the models.dirs setting, ...). Skips without a GPU.

const fs = require('node:fs');

const W = 640, H = 480, FRAMES = 4;
const WAIT_MS = 120000;

function birdAt(i) { return { cx: 300 + 14 * i, cy: 236 - 6 * i }; }

function inBird(i, x, y) {
    const { cx, cy } = birdAt(i);
    const body = ((x - cx) / 70) ** 2 + ((y - cy) / 42) ** 2 < 1;
    const head = (x - (cx + 58)) ** 2 + (y - (cy - 30)) ** 2 < 26 * 26;
    return body || head;
}

function makeFrame(i) {
    const data = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const k = (y * W + x) * 4;
            let r = 70 + 60 * x / W, g = 110 + 50 * y / H, b = 150 - 40 * x / W;
            if (Math.abs((y - 300) - 0.25 * (x - 320)) < 9) { r = 92; g = 64; b = 40; }
            if (inBird(i, x, y)) {
                const { cx, cy } = birdAt(i);
                const head = (x - (cx + 58)) ** 2 + (y - (cy - 30)) ** 2 < 26 * 26;
                if (head) { r = 60; g = 52; b = 48; } else { r = 214; g = 120; b = 38; }
            }
            data[k] = r; data[k + 1] = g; data[k + 2] = b; data[k + 3] = 255;
        }
    }
    return { width: W, height: H, data };
}

function inBody(i, x, y) {
    const { cx, cy } = birdAt(i);
    return ((x - cx) / 70) ** 2 + ((y - cy) / 42) ** 2 < 1;
}

// How well a 0/1 mask covers frame i's bird: the smaller of its precision
// (the fraction of the mask on the bird) and its recall of the body. A click
// on the orange body may give the body alone or body + dark head; both are the
// bird, so neither is penalised, while a mask spilling onto the background or
// missing the body is.
function birdIoU(i, mask) {
    let on = 0, total = 0, body = 0, bodyHit = 0;
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const m = mask[y * W + x] === 1;
            if (m) { total++; if (inBird(i, x, y)) on++; }
            if (inBody(i, x, y)) { body++; if (m) bodyHit++; }
        }
    }
    return Math.min(total ? on / total : 0, body ? bodyHit / body : 0);
}

function pump(pred, what) {
    const t0 = Date.now();
    while (Date.now() - t0 < WAIT_MS) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(2);
    }
    assert(false, what + ' did not complete within ' + WAIT_MS + 'ms');
    return false;
}

function findWeights() {
    const env = process.env.SAM2_DIR;
    if (env) return env;
    return bro.models ? bro.models.find('sam2.1-hiera-base-plus') : null;
}

const dir = findWeights();
if (!bro.vision || bro.vision.available === false) {
    skipTest('bro.vision is the stub');
} else if (!bro.gpu || !bro.gpu.available) {
    skipTest('no GPU backend');
} else if (!dir || !fs.existsSync(dir + '/model.safetensors')) {
    skipTest('no sam2.1-hiera-base-plus (set SAM2_DIR or BRO_MODELS)');
} else {
    const sam2 = bro.vision.loadSam2(dir);
    assert(sam2.device !== 'CPU', 'loadSam2 lands on the GPU (' + sam2.device + ')');
    assert(sam2.hasImage === false, 'no image before setImage');
    const frames = [];
    for (let i = 0; i < FRAMES; i++) frames.push(makeFrame(i));

    // ── Image predictor ──
    sam2.setImage(frames[0]);
    assert(sam2.hasImage, 'hasImage after setImage');
    let threw = null;
    try { sam2.segment({}); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'segment({}) is a TypeError');

    const seg = sam2.segment({ points: [[300, 236]], labels: [1] });
    assert(seg.num === 3 && seg.masks.length === 3, 'one click gives three proposals');
    assert(seg.width === W && seg.height === H, 'masks at the input resolution');
    const best = seg.masks[seg.best];
    assert(best.data.length === W * H && best.logits.length === W * H, 'mask bytes and logits');
    assert(best.lowRes.length === 256 * 256, 'lowRes logits for refinement');
    assert(best.image && best.image.width === W, 'a drawable image per mask');
    const iouClick = birdIoU(0, best.data);
    console.log('  click: best mask covers the drawn bird ' + iouClick.toFixed(3) +
                ', predicted ' + best.iou.toFixed(3) + ', objectScore ' + seg.objectScore.toFixed(2));
    assert(iouClick > 0.85, 'the click finds the bird');

    const boxSeg = sam2.segment({ box: [226, 182, 382, 292], multimask: false });
    assert(boxSeg.num === 1, 'box with multimask:false gives one mask');
    assert(birdIoU(0, boxSeg.masks[0].data) > 0.85, 'the box finds the bird');
    const b = boxSeg.masks[0].box;
    assert(b && b[0] < 240 && b[2] > 370 && b[1] < 200 && b[3] > 270, 'the mask box spans the bird: ' + b);

    // Refinement: feed a mask's low-res logits back with a second click.
    const refined = sam2.segment({ points: [[300, 236], [120, 300]], labels: [1, 0],
                                   multimask: false, maskInput: best.lowRes });
    assert(refined.num === 1 && birdIoU(0, refined.masks[0].data) > 0.85, 'maskInput refinement');

    // ── Tracker ──
    const trk = sam2.tracker();
    for (let i = 0; i < FRAMES; i++) {
        const idx = trk.addFrame(frames[i]);
        assert(idx === i, 'addFrame returns the frame index');
    }
    assert(trk.numFrames === FRAMES, 'numFrames');
    threw = null;
    try { trk.prompt(FRAMES, { points: [[1, 1]] }); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, 'prompt on a frame not added is a RangeError');

    const p0 = trk.prompt(0, { points: [[300, 236]] });
    assert(p0.conditioning && p0.present, 'frame 0 prompted and present');
    assert(birdIoU(0, p0.data) > 0.85, 'frame 0 mask is the bird');

    const all = trk.propagate();
    assert(all.length === FRAMES, 'propagate returns every frame');
    let lastCx = -1;
    for (const r of all) {
        assert(r.present && r.tracked && r.box, 'frame ' + r.frame + ' tracked and present');
        if (!r.box) continue;
        const iou = birdIoU(r.frame, r.data);
        const cx = (r.box[0] + r.box[2]) / 2;
        console.log('  frame ' + r.frame + ': cover ' + iou.toFixed(3) + ' score ' +
                    r.objectScore.toFixed(2) + ' box ' + r.box.join(','));
        assert(iou > 0.8, 'frame ' + r.frame + ' follows the bird');
        assert(cx > lastCx, 'the box moves with the bird');
        assert(r.image && r.image.width === W, 'frame result has a drawable image');
        lastCx = cx;
    }

    // Correction on a tracked frame, then propagate again — async.
    const fix = trk.prompt(2, { points: [[362, 286]], labels: [0] });
    assert(fix.conditioning && fix.present, 'the correction keeps the bird');
    let done = null, info = null;
    const handle = trk.propagate({ onDone(r, i) { done = r; info = i; } });
    assert(handle && typeof handle.cancel === 'function', 'async propagate returns a handle');
    threw = null;
    try { trk.track(1); } catch (e) { threw = e; }
    assert(threw && String(threw.message).includes('in flight'), 'one op at a time per model');
    threw = null;
    try { sam2.segment({ points: [[1, 1]] }); } catch (e) { threw = e; }
    assert(threw && String(threw.message).includes('in flight'), 'the model shares the guard');
    pump(() => done !== null || (info && info.error), 'async propagate');
    assert(!info.error && !info.cancelled, 'async propagate succeeded: ' + (info && info.error));
    assert(done.length === FRAMES && done[2].conditioning, 'frame 2 is now a conditioning frame');
    for (const r of done) assert(birdIoU(r.frame, r.data) > 0.8, 're-propagated frame ' + r.frame);

    // result() reads back without recomputing; reverse tracking runs too.
    const r3 = trk.result(3);
    assert(r3.tracked && r3.frame === 3, 'result(3)');
    const back = trk.track(1, { reverse: true });
    assert(back.present && birdIoU(1, back.data) > 0.8, 'reverse track into frame 1');

    trk.reset();
    assert(trk.result(0).tracked === false, 'reset forgets the prompts');
}
console.log('PASS');

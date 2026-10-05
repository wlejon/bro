// The post chain's settings, by their pixels:
//   - bloom: the glow's reach follows `radius` (the blur strength) and its
//     onset is soft — a highlight just under the threshold adds nothing, one
//     past it glows;
//   - tilt-shift: a horizontal band around `focus` stays sharp while the
//     frame above and below it blurs;
//   - unlit meshes draw over the tonemapped frame, so their authored colour
//     comes out exactly whatever the tonemap operator and exposure.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 5], target: [0, 0, 0] });
        sc.createLight({ type: 'directional', intensity: 0 });
        sc.setAmbient({ color: [0, 0, 0] });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

function boxMean(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    }
    return sum / n;
}

// Spread of one channel along each row of a band: high where stripes are
// sharp, low where they are blurred together.
function bandContrast(img, y0, y1) {
    let total = 0;
    for (let y = y0; y < y1; y++) {
        let mean = 0;
        for (let x = 0; x < img.width; x++) mean += img.data[(y * img.width + x) * 4];
        mean /= img.width;
        let v = 0;
        for (let x = 0; x < img.width; x++) {
            const d = img.data[(y * img.width + x) * 4] - mean;
            v += d * d;
        }
        total += Math.sqrt(v / img.width);
    }
    return total / (y1 - y0);
}

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 128;

    // =====================================================================
    // Bloom: a small emissive square on black. 5 units away with a 60° fov
    // there are ~22 px per unit; the square is 0.5 units, ~11 px.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        const square = sc.createMesh({ mesh: Mesh.box(0.25, 0.25, 0.05), color: [1, 1, 1, 1], emissive: 4 });
        // The glow 14-18 px right of the square's centre: clear of the square.
        const halo = (img) => boxMean(img, 0, 64 + 14, 60, 64 + 18, 68);

        const off = sc.captureFrame();
        assert(halo(off) < 2, `no bloom: no halo (${halo(off).toFixed(1)})`);

        sc.setBloom({ threshold: 1, intensity: 1, radius: 0.5 });
        const tight = halo(sc.captureFrame());
        sc.setBloom({ threshold: 1, intensity: 1, radius: 4 });
        const wide = halo(sc.captureFrame());
        assert(wide > tight + 8, `a larger radius reaches further: ${tight.toFixed(1)} -> ${wide.toFixed(1)}`);

        // Soft knee: luminance 0.9 under a threshold of 1 adds nothing, 1.8
        // adds most of its energy.
        square.emissive = 0.9;
        sc.setBloom({ threshold: 1, intensity: 1, radius: 4 });
        const under = halo(sc.captureFrame());
        square.emissive = 1.8;
        const over = halo(sc.captureFrame());
        assert(under < 2, `a highlight under the threshold does not bloom (${under.toFixed(1)})`);
        assert(over > under + 5, `one over the threshold does (${over.toFixed(1)})`);
        dropScene(s);
    }

    // =====================================================================
    // Tilt-shift: 4 px vertical stripes across the whole frame, focus band
    // in the middle.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        const stripes = new Uint8Array(32 * 4);
        for (let x = 0; x < 32; x++) {
            const v = x % 2 ? 255 : 0;
            stripes.set([v, v, v, 255], x * 4);
        }
        const wall = sc.createMesh({ mesh: Mesh.box(2.9, 2.9, 0.05), color: [1, 1, 1, 1], unlit: true });
        wall.setBaseColorTexture({ width: 32, height: 1, data: stripes });

        const sharp = sc.captureFrame();
        const top0 = bandContrast(sharp, 4, 20), mid0 = bandContrast(sharp, 56, 72);
        assert(top0 > 60 && mid0 > 60, `stripes start sharp: top ${top0.toFixed(0)}, middle ${mid0.toFixed(0)}`);

        sc.setTiltShift({ focus: 0.5, range: 0.1, blur: 1 });
        const tilted = sc.captureFrame();
        const top = bandContrast(tilted, 4, 20), mid = bandContrast(tilted, 56, 72);
        const bottom = bandContrast(tilted, 108, 124);
        assert(mid > mid0 * 0.8, `the focus band stays sharp: ${mid.toFixed(0)} (was ${mid0.toFixed(0)})`);
        assert(top < mid * 0.6 && bottom < mid * 0.6,
            `above and below it blur: top ${top.toFixed(0)}, bottom ${bottom.toFixed(0)}, band ${mid.toFixed(0)}`);
        dropScene(s);
    }

    // =====================================================================
    // Unlit overlay: authored colour exact under ACES at exposure 3.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setToneMap({ mode: 'aces', exposure: 3.0 });
        sc.createMesh({ mesh: Mesh.box(1, 1, 0.1), color: '#4080c0', unlit: true });
        const img = sc.captureFrame();
        const c = [0, 1, 2].map((ch) => boxMean(img, ch, 60, 60, 68, 68));
        const want = [0x40, 0x80, 0xc0];
        assert(c.every((v, i) => Math.abs(v - want[i]) <= 2),
            `an unlit mesh keeps its authored colour: (${c.map((v) => v.toFixed(0))}) want (${want})`);
        dropScene(s);
    }

    console.log('test_scene_postfx: OK');
}

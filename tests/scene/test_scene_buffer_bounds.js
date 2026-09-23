// Scene natives that copy a script's pixel or splat buffer check its length
// against the extent they were given, rather than reading past its end:
// setBaseColorTexture / setEmissionTexture (RGBA8, width*height*4 bytes),
// setShaderTexture (width*height floats), setCloud (every stream sized for
// the same N splats; the splat pass indexes each one per splat), and the
// clipmap layer natives (sides multiplied in 64 bits).

function throwsKind(fn, Kind) {
    try { fn(); } catch (e) { return e instanceof Kind; }
    return false;
}

const cv = document.createElement('canvas');
cv.setAttribute('width', '64');
cv.setAttribute('height', '64');
document.body.appendChild(cv);
flush();
const scene = cv.getContext('scene');

if (!scene) {
    console.log('no scene context (no GPU); skipping scene buffer bounds test');
} else {
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
    const box = scene.createMesh({ mesh: 'box', color: '#ffffff' });

    // ---- RGBA8 textures --------------------------------------------------
    assert(throwsKind(() => box.setBaseColorTexture({ data: new Uint8Array(16), width: 64, height: 64 }), RangeError),
        'setBaseColorTexture: short data is a RangeError');
    assert(throwsKind(() => box.setEmissionTexture({ data: new Uint8Array(16), width: 64, height: 64 }), RangeError),
        'setEmissionTexture: short data is a RangeError');
    box.setBaseColorTexture({ data: new Uint8Array(2 * 2 * 4).fill(255), width: 2, height: 2 });
    box.setEmissionTexture({ data: new Uint8Array(2 * 2 * 4), width: 2, height: 2 });
    box.setBaseColorTexture(null);
    box.setEmissionTexture(null);

    // A view keeps its own window: 2x2 RGBA taken from the middle of a
    // larger buffer is exactly 16 bytes and uploads.
    const big = new Uint8Array(64).fill(7);
    box.setBaseColorTexture({ data: big.subarray(16, 32), width: 2, height: 2 });
    assert(throwsKind(() => box.setBaseColorTexture({ data: big.subarray(16, 24), width: 2, height: 2 }), RangeError),
        'setBaseColorTexture reads a subarray\'s length, not its whole buffer');

    // ---- Custom shader sampler -------------------------------------------
    assert(throwsKind(() => box.setShaderTexture('u_field', { data: new Float32Array(4), width: 1000, height: 1000 }), RangeError),
        'setShaderTexture: short data is a RangeError');
    box.setShaderTexture('u_field', { data: new Float32Array(16), width: 4, height: 4 });
    assert(throwsKind(() => box.setShaderTexture('u_field', { data: new Float32Array(1), x: 0, y: 0, width: 4, height: 4 }), RangeError),
        'setShaderTexture sub-rect: short data is a RangeError');
    box.setShaderTexture('u_field', { data: new Float32Array(4), x: 1, y: 1, width: 2, height: 2 });

    // ---- Gaussian splat cloud --------------------------------------------
    const N = 3;
    const whole = () => ({
        positions: new Float32Array(N * 3),
        scales: new Float32Array(N * 3).fill(0.1),
        rotations: new Float32Array(N * 4).map((_, i) => (i % 4 === 3 ? 1 : 0)),
        opacities: new Float32Array(N).fill(1),
        sh: new Float32Array(N * 3),
        shDegree: 0,
    });
    const splat = scene.createGaussianSplat({ cloud: whole() });
    assert(splat.splatCount === N, 'a whole cloud loads');
    assert(throwsKind(() => splat.setCloud({ positions: new Float32Array(N * 3) }), TypeError),
        'setCloud: positions alone is a TypeError');
    const shortScales = whole(); shortScales.scales = new Float32Array(3);
    assert(throwsKind(() => splat.setCloud(shortScales), TypeError),
        'setCloud: a short stream is a TypeError');
    assert(splat.splatCount === N, 'a refused cloud leaves the old one in place');
    const highDeg = whole(); highDeg.shDegree = 9;   // clamps to 3: N*16*3 sh
    assert(throwsKind(() => splat.setCloud(highDeg), TypeError),
        'setCloud: sh sized for degree 0 does not pass as degree 3');
    highDeg.sh = new Float32Array(N * 16 * 3);
    splat.setCloud(highDeg);
    assert(splat.splatCount === N, 'shDegree above 3 clamps to 3');
    scene.captureFrame();

    // ---- Clipmap layers ----------------------------------------------------
    const cm = scene.createClipmapTerrain({ levels: 2, resolution: 16, cellSize: 1.0, detailOctaves: 99 });
    // 65536 * 65537 wraps to 65536 in 32-bit int; the layer must not take
    // a 1-float array for it.
    cm.setHeightLayer(0, { data: new Float32Array(65536), width: 65536, height: 65537 });
    assert(cm.layerCount === 0, 'setHeightLayer: an overflowing extent is refused');
    cm.setHeightLayer(0, { data: new Float32Array(16), width: 4, height: 4 });
    assert(cm.layerCount === 1, 'a height layer that fits loads');
    cm.setDetail({ octaves: -5 });
    cm.update(0, 10, 0);
    scene.captureFrame();
    cm.destroy();
}

console.log('scene buffer bounds test done');

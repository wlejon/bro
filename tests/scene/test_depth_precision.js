// Depth precision across an extreme near/far ratio.
//
// A world that runs from a metre underfoot to a planet on the horizon needs a
// depth buffer that still separates surfaces at the far end. This is the
// property reversed-Z exists for, so test the property rather than the
// mechanism: put two large quads 20 m apart at 150 km under a 4,000,000:1
// near/far ratio and require the nearer one to occlude the further one
// cleanly. Conventional 24-bit depth cannot resolve 20 m out of 150 km at
// that ratio and speckles the overlap; reversed-Z resolves it comfortably.
//
// Both quads are emissive with a zero-intensity light so the readback colours
// are deterministic and don't depend on the shading path.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '200');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

// BRO_DISABLE_REVERSED_Z forces the conventional fallback so that path stays
// testable on hardware that does support clip control. Under it this test is
// measuring a capability that has been deliberately switched off, so skip
// rather than report a failure. A machine that genuinely lacks
// ARB_clip_control still fails here, which is the honest signal — it cannot
// render correctly at these ranges.
const forcedOff = typeof process !== 'undefined' && process.env &&
                  process.env.BRO_DISABLE_REVERSED_Z &&
                  process.env.BRO_DISABLE_REVERSED_Z !== '0';

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping depth precision test');
} else if (forcedOff) {
    console.log('reversed-Z forced off; skipping depth precision test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.5, far: 2000000,
        position: [0, 0, 0], target: [0, 0, -1], up: [0, 1, 0],
    });
    scene.createLight({ type: 'directional', intensity: 0 });

    const NEAR_Z = -150000;
    const GAP    = 20;

    // Wide enough to cover the middle of the frame at this distance: the view
    // is ~173 km tall at 150 km with a 60 degree vertical FOV.
    scene.createMesh({
        mesh: Mesh.box(60000, 60000, 10), color: [1, 0, 0, 1], emissive: 1,
        z: NEAR_Z,
    });
    scene.createMesh({
        mesh: Mesh.box(60000, 60000, 10), color: [0, 1, 0, 1], emissive: 1,
        z: NEAR_Z - GAP,
    });

    flush();
    const img = scene.captureFrame();

    // Scan the middle of the frame, where both quads project. Every pixel
    // must be the near (red) quad; any green is the far quad punching through.
    let red = 0, green = 0, other = 0;
    for (let y = 60; y < 140; y++) {
        for (let x = 60; x < 140; x++) {
            const i = (y * img.width + x) * 4;
            const r = img.data[i], g = img.data[i + 1];
            if (r > 200 && g < 60)      red++;
            else if (g > 200 && r < 60) green++;
            else                        other++;
        }
    }

    console.log('depth precision: red=' + red + ' green=' + green +
                ' other=' + other);

    assert(red > 6000, 'near quad covers the sample window (red=' + red + ')');
    assert(green === 0,
           'far quad never punches through 20 m of separation at 150 km ' +
           '(green=' + green + ' pixels)');

    console.log('DEPTH PRECISION OK');
}

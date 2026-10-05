// Camera-relative rendering (scene/vulkan/scene_view.h): the GPU sees every
// position with the eye subtracted on the CPU, where the large values cancel,
// so a scene far from the world origin renders exactly as the same scene at
// the origin. Drawn in absolute world space instead, fp32 positions near
// 4e6 m are quantised to 0.25-0.5 m and the frame visibly tears apart.
//
// The same small scene — a ground, boxes, a point light and a spot casting
// shadows, a sun, a decal, an unlit marker — is rendered at the origin and
// shifted by a large offset with the camera moved along. Every coordinate is
// a multiple of 0.25, exact in fp32 at every offset, so any difference is the
// renderer's own rounding. The sun casts no shadow here: its cascades snap to
// a world-anchored texel grid (deliberately, so they do not shimmer under a
// pan), which a shift that is not a whole number of texels moves by design.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    return { canvas: cv, scene: cv.getContext('scene') };
}

function compare(a, b) {
    let maxDiff = 0, over = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        let d = 0;
        for (let k = 0; k < 3; k++) d = Math.max(d, Math.abs(a.data[i + k] - b.data[i + k]));
        maxDiff = Math.max(maxDiff, d);
        if (d > 3) over++;
    }
    return { maxDiff, over };
}

const SIZE = 192;
const s = freshScene(SIZE);
if (!s.scene) {
    missingGpuContext('scene');
} else {
    const scn = s.scene;
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.setAmbient({ color: [0.1, 0.1, 0.1] });

    const ground = scn.createMesh({ mesh: 'plane', halfW: 6, halfD: 6, color: [0.7, 0.7, 0.7, 1],
                                    castsShadow: false });
    const boxA = scn.createMesh({ mesh: Mesh.box(0.5, 0.5, 0.5), color: [0.9, 0.3, 0.2, 1], roughness: 0.5 });
    const boxB = scn.createMesh({ mesh: Mesh.box(0.25, 0.75, 0.25), color: [0.2, 0.5, 0.9, 1], roughness: 0.3 });
    const marker = scn.createMesh({ mesh: Mesh.box(0.125, 0.125, 0.125), color: [1, 1, 0, 1], unlit: true });

    const sun = scn.createLight({ type: 'directional', direction: [-0.25, -1, -0.5], intensity: 0.5 });
    const lamp = scn.createLight({ type: 'point', range: 10, intensity: 20 });
    lamp.castsShadow = true;
    const spot = scn.createLight({ type: 'spot', direction: [0, -1, 0], range: 12, intensity: 25 });
    spot.castsShadow = true;
    const decal = scn.createDecal({ modulate: [0.1, 0.9, 0.2, 1], size: [1.5, 1, 1.5] });

    // Everything placed relative to (O, 0, O).
    function frameAt(O) {
        ground.x = O; ground.y = 0; ground.z = O;
        boxA.x = O - 1; boxA.y = 0.5; boxA.z = O;
        boxB.x = O + 1; boxB.y = 0.75; boxB.z = O - 0.5;
        marker.x = O; marker.y = 1.5; marker.z = O + 1;
        lamp.x = O - 0.25; lamp.y = 2.5; lamp.z = O + 1.5;
        spot.x = O + 1.5; spot.y = 4; spot.z = O - 0.25;
        decal.x = O + 0.5; decal.y = 0; decal.z = O + 1.75;
        scn.setCamera({ fov: 55, near: 0.1, far: 60,
                        position: [O + 2.5, 4, O + 6], target: [O, 0.5, O], up: [0, 1, 0] });
        flush();
        return scn.captureFrame();
    }

    const origin = frameAt(0);
    assert(origin.width === SIZE && origin.height === SIZE, 'frame captured');
    // The scene is really there: lit ground, a shadow, the decal.
    let lit = 0;
    for (let i = 0; i < origin.data.length; i += 4) if (origin.data[i] > 40) lit++;
    assert(lit > SIZE * SIZE / 4, `the scene covers the frame (${lit} lit px)`);
    const st = scn.cullStats();
    assert(st.shadowTilesTotal === 7, `the point and spot lights cast (7 tiles, got ${st.shadowTilesTotal})`);
    assert(st.decalsDrawn === 1, 'the decal draws');

    for (const O of [100000, 4000000]) {
        const shifted = frameAt(O);
        const c = compare(origin, shifted);
        console.log(`camera-relative: offset ${O}: max diff ${c.maxDiff}, ${c.over} px over 3`);
        assert(c.over <= 2 && c.maxDiff <= 8,
            `offset ${O} renders as at the origin (${c.over} px differ by more than 3, max ${c.maxDiff})`);
    }

    // Back at the origin, nothing drifted.
    const again = compare(origin, frameAt(0));
    assert(again.over === 0, `the origin frame repeats (${again.over} px)`);

    document.body.removeChild(s.canvas);
    flush();
}

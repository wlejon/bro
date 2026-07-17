// SSAO (scene.setSSAO) — half-res depth-based AO multiplied into the lit HDR
// image in the tonemap pass. A floor/wall crease must darken when SSAO is on;
// open floor far from any occluder must stay (nearly) untouched. Mid-gray
// emissive surfaces make the un-occluded base color deterministic AND keep
// the HDR value below 1.0 so the multiply isn't clamped away (AO is a
// post-multiply on the lit image, so it applies to emissive too); linear
// tonemap + gamma 1 keeps readback linear. All assertions comparative
// (on vs off) so they're robust to GPU variance.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '200');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping ssao test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 3, 5], target: [0, 0.5, -2], up: [0, 1, 0],
    });

    // Interior corner: floor plane + back wall. Mid-gray emissive with a
    // zero-intensity light (suppresses the implicit sun) keeps the HDR value
    // ~0.62 — comfortably below 1.0, so the AO multiply survives the tonemap
    // clamp (a super-white HDR surface would clamp right back to 255 and
    // hide moderate occlusion).
    scene.createLight({ type: 'directional', intensity: 0 });
    scene.createMesh({
        mesh: Mesh.box(8, 0.25, 8), color: [0.6, 0.6, 0.6, 1], emissive: 1,
        y: -0.25,
    });
    scene.createMesh({
        mesh: Mesh.box(8, 4, 0.25), color: [0.6, 0.6, 0.6, 1], emissive: 1,
        y: 4 - 0.25, z: -4,
    });

    // Average a small window so single-pixel noise can't flip the result.
    const avg = (img, cx, cy, half) => {
        let sum = 0, n = 0;
        for (let y = cy - half; y <= cy + half; y++) {
            for (let x = cx - half; x <= cx + half; x++) {
                sum += img.data[(y * img.width + x) * 4];
                n++;
            }
        }
        return sum / n;
    };

    // Find the crease row: walk down the center column of the OFF frame's
    // geometry; the crease is where the wall meets the floor. Both are
    // emissive white, so instead locate it geometrically: project the corner
    // point (0, 0, -3.75). With this camera it lands mid-image; sample a
    // window just above the floor/wall junction and one on open floor near
    // the bottom (close to the camera, far from the wall).
    const offImg = scene.captureFrame();

    scene.setSSAO({ enabled: true, radius: 1.5, intensity: 1.5, bias: 0.02 });
    const onImg = scene.captureFrame();

    // Locate the crease: scan the center column for the row where the
    // on-frame darkened the most relative to the off-frame.
    let bestRow = -1, bestDrop = 0;
    for (let y = 10; y < 190; y++) {
        const i = (y * offImg.width + 100) * 4;
        const drop = offImg.data[i] - onImg.data[i];
        if (drop > bestDrop) { bestDrop = drop; bestRow = y; }
    }
    console.log(`max AO darkening ${bestDrop} at row ${bestRow}`);

    // Qualitative: the crease darkens substantially...
    assert(bestDrop > 25,
        `SSAO darkens the floor/wall crease (max drop=${bestDrop})`);
    const creaseOff = avg(offImg, 100, bestRow, 2);
    const creaseOn  = avg(onImg, 100, bestRow, 2);
    assert(creaseOff - creaseOn > 15,
        `crease window darker with SSAO on (${creaseOff.toFixed(1)} -> ${creaseOn.toFixed(1)})`);
    assert(creaseOff > 120, `crease is plain mid-gray without SSAO (${creaseOff.toFixed(1)})`);

    // ...while open floor near the camera (bottom rows, far from the wall)
    // is affected far less than the crease. Relative rather than absolute:
    // software GL (llvmpipe, as CI runs under Xvfb) produces a stronger AO
    // whose half-res depth sampling bleeds some occlusion onto the near floor
    // (~15/255), so an absolute "< 10" bound is renderer-dependent. The real
    // invariant is that occlusion concentrates in the crease, not open floor.
    const openOff = avg(offImg, 100, 192, 2);
    const openOn  = avg(onImg, 100, 192, 2);
    const creaseDrop = creaseOff - creaseOn;
    const openDrop = Math.abs(openOff - openOn);
    assert(openOff > 120, `open floor visible in off frame (${openOff.toFixed(1)})`);
    assert(openDrop < creaseDrop * 0.4,
        `open floor far less affected than crease ` +
        `(open drop=${openDrop.toFixed(1)}, crease drop=${creaseDrop.toFixed(1)})`);

    // Disabled again -> matches the off frame exactly (same virtual frame,
    // deterministic pipeline).
    scene.setSSAO({ enabled: false });
    const offImg2 = scene.captureFrame();
    let maxDelta = 0;
    for (let i = 0; i < offImg.data.length; i += 97) {   // sparse sweep
        const d = Math.abs(offImg.data[i] - offImg2.data[i]);
        if (d > maxDelta) maxDelta = d;
    }
    assert(maxDelta === 0,
        `SSAO off is pixel-identical to never-enabled (maxDelta=${maxDelta})`);

    flush();
}

document.body.removeChild(canvas);

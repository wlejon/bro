// Projected decals (Godot Decal analog) — scene.createDecal. The decal
// volume is the unit box scaled by the node's scale, projecting along local
// -Y; the fragment pass reconstructs opaque scene positions from the depth
// snapshot and blends albedo * modulate onto the lit HDR result. Verifies:
//   - a decal projected onto a floor plane tints the probed pixel; pixels
//     outside the box footprint keep the floor color
//   - rotating the decal node reorients the projection (asymmetric texture
//     flips sides under ry=180; a thin box footprint moves under rotation)
//   - upperFade / lowerFade reduce alpha toward the volume ends
//   - normalFade cuts surfaces facing away from the projection direction
//     (wall front face) while the floor keeps the decal
//   - renderPriority orders overlapping decals (live-editable)
//   - ortho cameras reconstruct depth correctly (decal still lands)
//   - emission adds self-lit color independent of albedo alpha
//   - frustum culling counts decals (cullStats) and property round-trips
// Exercises SceneRenderer::renderDecalPass (scene_renderer_decals.cpp),
// DecalNode (decal_node.h) and the createDecal/property bindings.

function patchChannelAvg(img, cx, cy, r, ch) {
    let sum = 0, n = 0;
    cx = Math.round(cx); cy = Math.round(cy);
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

// Column-major mat4 * vec4 (matches bromath Mat4.data layout exposed by
// scene.viewMatrix / scene.projectionMatrix).
function mulMat4Vec4(m, v) {
    const out = [0, 0, 0, 0];
    for (let r = 0; r < 4; r++)
        out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    return out;
}

// World point -> canvas pixel through the scene's current matrices.
function projectToScreen(scn, size, p) {
    const e = mulMat4Vec4(scn.viewMatrix, [p[0], p[1], p[2], 1]);
    const c = mulMat4Vec4(scn.projectionMatrix, e);
    const ndcX = c[0] / c[3], ndcY = c[1] / c[3];
    return [(ndcX * 0.5 + 0.5) * size, (1 - (ndcY * 0.5 + 0.5)) * size];
}

function solidTex(w, h, r, g, b, a) {
    const data = new Uint8Array(w * h * 4);
    for (let i = 0; i < w * h; i++) {
        data[i * 4] = r; data[i * 4 + 1] = g; data[i * 4 + 2] = b; data[i * 4 + 3] = a;
    }
    return { width: w, height: h, data };
}

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    return { canvas: cv, scene: cv.getContext('scene') };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// Standard rig: gray floor, straight-down sun, camera on the +Z side above.
function setupFloorScene(scn) {
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0, -1, 0],
                      color: [1, 1, 1], intensity: 3 });
    scn.createMesh({ mesh: 'plane', halfW: 5, halfD: 5, color: [0.5, 0.5, 0.5] });
    scn.setCamera({ fov: 60, near: 0.1, far: 100,
                    position: [0, 6, 4], target: [0, 0, 0] });
}

const probe = freshScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping decal test');
} else {
    dropScene(probe);
    const S = 128;

    // =====================================================================
    // Section 1: basic projection + box containment + property surface.
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        setupFloorScene(scn);

        const decal = scn.createDecal({ modulate: 'red', size: [2, 2, 2] });
        assert(decal.type === 'decal', 'decal node type reads "decal"');
        assert(decal.renderPriority === 0, 'renderPriority defaults to 0');
        assert(Math.abs(decal.emissionStrength - 1) < 1e-6,
            'emissionStrength defaults to 1');
        assert(decal.upperFade === 0 && decal.lowerFade === 0 && decal.normalFade === 0,
            'fades default to 0 (off)');
        const mod = decal.modulate;
        assert(mod.length === 4 && mod[0] > 0.99 && mod[1] < 0.01,
            'modulate reads back as [r,g,b,a]');

        let img = scn.captureFrame();
        // Sanity: the world origin projects to the canvas center.
        const c = projectToScreen(scn, S, [0, 0, 0]);
        assert(Math.abs(c[0] - 64) < 1.5 && Math.abs(c[1] - 64) < 1.5,
            `origin projects to canvas center (got ${c[0].toFixed(1)},${c[1].toFixed(1)})`);

        const rIn = patchChannelAvg(img, 64, 64, 3, 0);
        const gIn = patchChannelAvg(img, 64, 64, 3, 1);
        assert(rIn > 150 && gIn < 60,
            `decal tints the floor red at its center (r=${rIn.toFixed(0)} g=${gIn.toFixed(0)})`);

        // 2 world units right of center — inside the floor, outside the box.
        const off = projectToScreen(scn, S, [2, 0, 0]);
        const rOut = patchChannelAvg(img, off[0], off[1], 3, 0);
        const gOut = patchChannelAvg(img, off[0], off[1], 3, 1);
        assert(Math.abs(rOut - gOut) < 12 && gOut > 60,
            `floor outside the box keeps its gray (r=${rOut.toFixed(0)} g=${gOut.toFixed(0)})`);

        // Culling: move the decal far behind the camera and check counters.
        img = scn.captureFrame();
        let stats = scn.cullStats();
        assert(stats.decalsDrawn === 1 && stats.decalsCulled === 0,
            `visible decal counts as drawn (${stats.decalsDrawn}/${stats.decalsCulled})`);
        decal.position = [0, 0, 500];
        img = scn.captureFrame();
        stats = scn.cullStats();
        assert(stats.decalsDrawn === 0 && stats.decalsCulled === 1,
            `out-of-frustum decal is culled (${stats.decalsDrawn}/${stats.decalsCulled})`);
        decal.position = [0, 0, 0];

        // visible=false removes it.
        decal.visible = false;
        img = scn.captureFrame();
        const rHidden = patchChannelAvg(img, 64, 64, 3, 0);
        const gHidden = patchChannelAvg(img, 64, 64, 3, 1);
        assert(Math.abs(rHidden - gHidden) < 12,
            `hidden decal leaves the floor gray (r=${rHidden.toFixed(0)} g=${gHidden.toFixed(0)})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 2: rotation reorients the projection.
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        setupFloorScene(scn);

        // Asymmetric 2x1 texture: left texel red, right texel blue. U maps
        // local +X, so world -X shows red until the node spins.
        const tex = { width: 2, height: 1,
                      data: new Uint8Array([255, 0, 0, 255,  0, 0, 255, 255]) };
        const decal = scn.createDecal({ texture: tex, size: [2, 2, 2] });

        const left = projectToScreen(scn, S, [-0.5, 0, 0]);
        let img = scn.captureFrame();
        let r = patchChannelAvg(img, left[0], left[1], 2, 0);
        let b = patchChannelAvg(img, left[0], left[1], 2, 2);
        assert(r > 120 && r > b * 2,
            `left half shows the red texel (r=${r.toFixed(0)} b=${b.toFixed(0)})`);

        // Spin 180 about the projection axis: the same world point now
        // samples the blue texel — the projection followed the node.
        decal.rotationY = Math.PI;
        img = scn.captureFrame();
        r = patchChannelAvg(img, left[0], left[1], 2, 0);
        b = patchChannelAvg(img, left[0], left[1], 2, 2);
        assert(b > 120 && b > r * 2,
            `after ry=180 the same point shows blue (r=${r.toFixed(0)} b=${b.toFixed(0)})`);

        // Thin-box footprint moves under rotation: a box thin in local Z
        // misses world z=-0.6 until a 90-degree yaw swings the long axis
        // onto it.
        scn.destroyNode(decal);
        const thin = scn.createDecal({ modulate: 'red', size: [2, 2, 0.5] });
        const p = projectToScreen(scn, S, [0, 0, -0.6]);
        img = scn.captureFrame();
        let rThin = patchChannelAvg(img, p[0], p[1], 2, 0);
        let gThin = patchChannelAvg(img, p[0], p[1], 2, 1);
        assert(Math.abs(rThin - gThin) < 12,
            `thin box misses z=-0.6 (r=${rThin.toFixed(0)} g=${gThin.toFixed(0)})`);
        thin.rotationY = Math.PI / 2;
        img = scn.captureFrame();
        rThin = patchChannelAvg(img, p[0], p[1], 2, 0);
        gThin = patchChannelAvg(img, p[0], p[1], 2, 1);
        assert(rThin > 150 && gThin < 60,
            `rotated thin box covers z=-0.6 (r=${rThin.toFixed(0)} g=${gThin.toFixed(0)})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 3: upperFade / lowerFade reduce alpha toward the volume ends.
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        setupFloorScene(scn);

        // Box spans y in [-1.9, 0.1]: the floor (y=0) sits near the +Y end
        // (local y = +0.45, i.e. t = 0.9 of the way to the top).
        const decal = scn.createDecal({ modulate: 'red', size: [2, 2, 2],
                                        y: -0.9 });
        let img = scn.captureFrame();
        const rFull = patchChannelAvg(img, 64, 64, 3, 0);
        assert(rFull > 150, `no fade: full-strength decal (r=${rFull.toFixed(0)})`);

        decal.upperFade = 8;
        assert(Math.abs(decal.upperFade - 8) < 1e-6, 'upperFade round-trips');
        img = scn.captureFrame();
        const rFaded = patchChannelAvg(img, 64, 64, 3, 0);
        const gFaded = patchChannelAvg(img, 64, 64, 3, 1);
        assert(Math.abs(rFaded - gFaded) < 15 && rFaded < rFull - 60,
            `upperFade=8 near the +Y end kills the decal (r=${rFaded.toFixed(0)} vs ${rFull.toFixed(0)})`);

        // Mirror for lowerFade: floor near the -Y end of the box.
        decal.upperFade = 0;
        decal.position = [0, 0.9, 0];
        img = scn.captureFrame();
        const rFull2 = patchChannelAvg(img, 64, 64, 3, 0);
        assert(rFull2 > 150, `repositioned, no fade: full strength (r=${rFull2.toFixed(0)})`);
        decal.lowerFade = 8;
        img = scn.captureFrame();
        const rFaded2 = patchChannelAvg(img, 64, 64, 3, 0);
        assert(rFaded2 < rFull2 - 60,
            `lowerFade=8 near the -Y end kills the decal (r=${rFaded2.toFixed(0)})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 4: normalFade cuts surfaces facing away from the projection.
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        setupFloorScene(scn);
        // The sun points straight down, so vertical faces get no NdotL —
        // raise the ambient so the wall-face probes have signal (this also
        // exercises the decal's ambient lighting term).
        scn.setAmbient([0.5, 0.5, 0.5]);

        // Wall inside the decal volume; its front (+Z) face is vertical, so
        // dot(surface normal, projection up) = 0 -> cut once normalFade > 0.
        scn.createMesh({ mesh: 'box', halfW: 0.6, halfH: 0.4, halfD: 0.3,
                         color: [0.5, 0.5, 0.5], y: 0.4 });
        const decal = scn.createDecal({ modulate: 'red', size: [2, 2, 2] });

        const wallFront = projectToScreen(scn, S, [0, 0.4, 0.3]);
        let img = scn.captureFrame();
        let r = patchChannelAvg(img, wallFront[0], wallFront[1], 2, 0);
        let g = patchChannelAvg(img, wallFront[0], wallFront[1], 2, 1);
        assert(r > 120 && g < 70,
            `normalFade off: the wall's vertical face receives the decal (r=${r.toFixed(0)} g=${g.toFixed(0)})`);

        decal.normalFade = 0.9;
        img = scn.captureFrame();
        r = patchChannelAvg(img, wallFront[0], wallFront[1], 2, 0);
        g = patchChannelAvg(img, wallFront[0], wallFront[1], 2, 1);
        assert(Math.abs(r - g) < 15,
            `normalFade=0.9 cuts the vertical face (r=${r.toFixed(0)} g=${g.toFixed(0)})`);

        // The wall's TOP face still faces the projection — decal stays.
        const wallTop = projectToScreen(scn, S, [0, 0.8, 0]);
        const rTop = patchChannelAvg(img, wallTop[0], wallTop[1], 2, 0);
        const gTop = patchChannelAvg(img, wallTop[0], wallTop[1], 2, 1);
        assert(rTop > 120 && gTop < 70,
            `normalFade keeps the upward-facing top (r=${rTop.toFixed(0)} g=${gTop.toFixed(0)})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 5: renderPriority orders overlapping decals (live-editable).
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        setupFloorScene(scn);

        const red = scn.createDecal({ modulate: [1, 0, 0, 1], size: [2, 2, 2] });
        const blue = scn.createDecal({ modulate: [0, 0, 1, 1], size: [2, 2, 2],
                                       renderPriority: 1 });
        let img = scn.captureFrame();
        let r = patchChannelAvg(img, 64, 64, 3, 0);
        let b = patchChannelAvg(img, 64, 64, 3, 2);
        assert(b > 150 && r < 60,
            `higher renderPriority draws on top (r=${r.toFixed(0)} b=${b.toFixed(0)})`);

        red.renderPriority = 2;
        assert(red.renderPriority === 2, 'renderPriority round-trips');
        img = scn.captureFrame();
        r = patchChannelAvg(img, 64, 64, 3, 0);
        b = patchChannelAvg(img, 64, 64, 3, 2);
        assert(r > 150 && b < 60,
            `raising priority flips the order (r=${r.toFixed(0)} b=${b.toFixed(0)})`);
        void blue;

        dropScene(s);
    }

    // =====================================================================
    // Section 6: ortho camera — depth reconstruction handles both
    // projection kinds.
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, -1, 0],
                          color: [1, 1, 1], intensity: 3 });
        scn.createMesh({ mesh: 'plane', halfW: 5, halfD: 5, color: [0.5, 0.5, 0.5] });
        scn.createCamera({ mode: 'ortho', size: 8, position: [0, 6, 4],
                           lookAt: [0, 0, 0], active: true });

        scn.createDecal({ modulate: 'red', size: [2, 2, 2] });
        const img = scn.captureFrame();
        const c = projectToScreen(scn, S, [0, 0, 0]);
        const r = patchChannelAvg(img, c[0], c[1], 3, 0);
        const g = patchChannelAvg(img, c[0], c[1], 3, 1);
        assert(r > 150 && g < 60,
            `ortho camera: decal lands on the floor (r=${r.toFixed(0)} g=${g.toFixed(0)})`);
        const off = projectToScreen(scn, S, [2.5, 0, 0]);
        const rOut = patchChannelAvg(img, off[0], off[1], 3, 0);
        const gOut = patchChannelAvg(img, off[0], off[1], 3, 1);
        assert(Math.abs(rOut - gOut) < 12,
            `ortho camera: outside the box stays gray (r=${rOut.toFixed(0)} g=${gOut.toFixed(0)})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 7: emission — self-lit, independent of albedo alpha, swappable
    // albedo at runtime via setBaseColorTexture.
    // =====================================================================
    {
        const s = freshScene(S);
        const scn = s.scene;
        setupFloorScene(scn);

        // Fully transparent albedo + green emission: only the emission shows.
        const decal = scn.createDecal({
            texture: solidTex(2, 2, 0, 0, 0, 0),
            emissionTexture: solidTex(2, 2, 0, 255, 0, 255),
            emissionStrength: 4,
            size: [2, 2, 2],
        });
        let img = scn.captureFrame();
        const gIn = patchChannelAvg(img, 64, 64, 3, 1);
        const off = projectToScreen(scn, S, [2, 0, 0]);
        const gOut = patchChannelAvg(img, off[0], off[1], 3, 1);
        assert(gIn > 240 && gIn > gOut + 60,
            `emission glows through zero-alpha albedo (in=${gIn.toFixed(0)} out=${gOut.toFixed(0)})`);

        // Runtime albedo swap through the shared setBaseColorTexture entry.
        decal.setBaseColorTexture(solidTex(2, 2, 255, 0, 0, 255));
        decal.emissionStrength = 0;
        img = scn.captureFrame();
        const r = patchChannelAvg(img, 64, 64, 3, 0);
        const g = patchChannelAvg(img, 64, 64, 3, 1);
        assert(r > 150 && g < 60,
            `setBaseColorTexture swaps decal albedo at runtime (r=${r.toFixed(0)} g=${g.toFixed(0)})`);

        dropScene(s);
    }

    console.log('decal tests passed');
}

// Scene-as-texture (SubViewport analog): sceneA.asTexture() returns a
// live-linked handle; mesh.setBaseColorTexture(handle) makes a mesh in
// scene B sample scene A's post-tonemap LDR output every frame. Exercises
// MeshNode's external texture slot (src/scene/mesh_node.h), the draw-time
// resolution + LINEAR/CLAMP params in scene_renderer_mesh.cpp, the
// SceneGraph::OutputTextureSource liveness token (scene_graph.h), and the
// asTexture / setBaseColorTexture(handle) bindings in scene_bindings.cpp.
// Covers: live link across source renderScale change (FBO/texture id
// recreated), source-scene destruction (fall back to base color, no stale
// GL id), source-never-rendered (untextured until first frame), clearing
// back to base color, replacing the link with owned pixel bytes, and
// self-sampling (allowed — verified not to crash).
//
// The consumer cube is unlit WHITE throughout: the sampled texture composes
// as texture * color, so a textured face shows the source's color exactly
// and an untextured fallback face is solid white — every state has a
// distinct channel signature without needing runtime color mutation.

function regionChannel(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

// [r, g, b] means over the center patch.
function centerRGB(img, r) {
    const cx = Math.floor(img.width / 2), cy = Math.floor(img.height / 2);
    return [0, 1, 2].map(ch => regionChannel(img, ch, cx - r, cy - r, cx + r, cy + r));
}

function assertFace(img, want, label) {
    const [r, g, b] = centerRGB(img, 8);
    const ok = (v, spec) => spec === 'hi' ? v > 150 : v < 40;
    assert(ok(r, want[0]) && ok(g, want[1]) && ok(b, want[2]),
        `${label}: rgb=(${r.toFixed(0)},${g.toFixed(0)},${b.toFixed(0)}) want [${want}]`);
}

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setCamera({ fov: 60, near: 0.1, far: 100,
                       position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// Screen-filling unlit colored quad — makes a scene's LDR output a solid
// color (linear tonemap + gamma 1 pass the authored color through exactly).
function fillQuad(sc, color, z = 0) {
    return sc.createMesh({ mesh: 'box', halfW: 6, halfH: 6, halfD: 0.1,
                           z, color, unlit: true });
}

const probe = freshScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping scene texture test');
} else {
    dropScene(probe);
    const SIZE = 128;

    // Source scene A (solid green). Created BEFORE the consumer so B samples
    // A same-frame (scenes render in getContext creation order — see
    // docs/scene-api.js asTexture()).
    const A = freshScene(64);
    fillQuad(A.scene, '#00ff00');
    flush();  // A renders once — its output texture now exists

    // Consumer scene B: an unlit white cube.
    const B = freshScene(SIZE);
    const cube = B.scene.createMesh({ mesh: 'box', color: '#ffffff', unlit: true });

    // =====================================================================
    // Basic link: B's cube face shows A's green.
    // =====================================================================
    const tex = A.scene.asTexture();
    assert(tex, 'asTexture returns a handle');
    assert(tex.valid === true, 'handle is valid while the source scene exists');
    cube.setBaseColorTexture(tex);
    assertFace(B.scene.captureFrame(), ['lo', 'hi', 'lo'],
        "cube face shows the source scene's green");

    // =====================================================================
    // Live link: changing the source's renderScale recreates its FBO chain
    // (new GL texture id) — the consumer picks up the new texture next
    // frame with no re-wiring, no black/garbage.
    // =====================================================================
    A.scene.setRenderScale(0.5);
    flush();  // A re-renders into the recreated (new-id) target
    assertFace(B.scene.captureFrame(), ['lo', 'hi', 'lo'],
        'link survives source renderScale change');
    A.scene.setRenderScale(2.0);
    flush();
    assertFace(B.scene.captureFrame(), ['lo', 'hi', 'lo'],
        'link survives a second target recreation');

    // =====================================================================
    // Source content is sampled live, not snapshotted.
    // =====================================================================
    fillQuad(A.scene, '#0000ff', 1);  // in front of the green quad
    flush();
    assertFace(B.scene.captureFrame(), ['lo', 'lo', 'hi'],
        "cube face tracks the source's new content (blue)");

    // =====================================================================
    // Source never rendered: a handle from a brand-new scene resolves to 0
    // until its first 3D frame — the consumer stays untextured (white base
    // color) and flips to textured once the source renders. Then destroying
    // the source falls back again, with no crash and no stale GL id.
    // =====================================================================
    {
        const cv = document.createElement('canvas');
        cv.setAttribute('width', '64');
        cv.setAttribute('height', '64');
        document.body.appendChild(cv);
        const C = cv.getContext('scene');
        C.setCamera({ fov: 60, near: 0.1, far: 100,
                      position: [0, 0, 4], target: [0, 0, 0] });
        C.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        fillQuad(C, '#ff00ff');

        const texC = C.asTexture();
        cube.setBaseColorTexture(texC);          // C has NOT rendered yet
        assert(texC.valid === true, 'unrendered source handle is still valid');
        assertFace(B.scene.captureFrame(), ['hi', 'hi', 'hi'],
            'unrendered source -> white base-color fallback');

        flush();                                 // C renders its magenta
        assertFace(B.scene.captureFrame(), ['hi', 'lo', 'hi'],
            'first source frame lights the link up (magenta)');

        document.body.removeChild(cv);
        flush();                                 // engine prunes C's graph
        assert(texC.valid === false, 'handle invalidates when its scene dies');
        assertFace(B.scene.captureFrame(), ['hi', 'hi', 'hi'],
            'destroyed source -> white base-color fallback');
    }

    // =====================================================================
    // Clearing: null restores the plain base color.
    // =====================================================================
    cube.setBaseColorTexture(tex);   // back to A (blue content)
    assertFace(B.scene.captureFrame(), ['lo', 'lo', 'hi'], 're-linked to A');
    cube.setBaseColorTexture(null);
    assertFace(B.scene.captureFrame(), ['hi', 'hi', 'hi'],
        'null clears the link back to base color');

    // =====================================================================
    // Mutual exclusion: owned pixel bytes replace a live link (and the
    // texture-only change applies without a geometry re-upload).
    // =====================================================================
    cube.setBaseColorTexture(tex);   // live link again
    assertFace(B.scene.captureFrame(), ['lo', 'lo', 'hi'], 'link before bytes swap');
    cube.setBaseColorTexture({ width: 1, height: 1,
                               data: new Uint8Array([255, 255, 0, 255]) });
    assertFace(B.scene.captureFrame(), ['hi', 'hi', 'lo'],
        'owned bytes replace the live link (yellow)');

    // =====================================================================
    // Source destroyed for good: A goes away; the handle reads invalid and
    // a mesh still linked to it renders base color.
    // =====================================================================
    cube.setBaseColorTexture(tex);
    dropScene(A);
    assert(tex.valid === false, 'handle invalidates when the source scene dies');
    assertFace(B.scene.captureFrame(), ['hi', 'hi', 'hi'],
        'destroyed A -> untextured white cube');

    // =====================================================================
    // Self-sampling (a scene texturing a mesh with ITSELF) is allowed: the
    // mesh pass samples last frame's tonemap output, never the FBO it is
    // rendering into, so there is no GL feedback loop. Verify it renders
    // repeatedly without crashing and keeps producing frames.
    // =====================================================================
    {
        const D = freshScene(64);
        const m = D.scene.createMesh({ mesh: 'box', color: '#ffffff', unlit: true });
        m.setBaseColorTexture(D.scene.asTexture());
        for (let i = 0; i < 4; i++) flush();
        const img = D.scene.captureFrame();
        assert(img && img.width === 64 && img.height === 64,
            'self-sampling scene keeps producing frames');
        dropScene(D);
    }

    dropScene(B);
    console.log('scene texture tests passed');
}

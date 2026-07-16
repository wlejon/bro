// Test scene.createSkinnedMesh — GPU skinning (SkinnedMeshNode).
// Exercises src/scene/skinned_mesh_node.cpp, the SKINNED variants of
// mesh.vert / shadow.vert, and the createSkinnedMesh / setSkinningMatrices
// bindings in src/js/scene_bindings.cpp.
//
// Procedural 2-bone test rig: a flat vertical strip (x in [-0.2,0.2],
// y in [0,2], z = 0, normals +z) split rigidly at y = 1 — lower half fully
// bone 0, upper half fully bone 1. Bone 1's hinge sits at (0,1,0):
//   - identity palette      -> renders exactly like a static createMesh copy
//   - 90 deg Z-bend palette -> the upper half swings to the horizontal arm
//                              x in [-1,0] at height y ~= 1
// The bend palette is produced through the REAL rigging pipeline
// (Skeleton -> Pose -> computeSkinningMatrices) so the documented recipe is
// what's under test, and the resulting matrices are also checked numerically
// against the analytic hinge transform. Shadows are verified to deform by
// comparing ground-plane brightness where the bent arm's shadow must land.

function patchMaxAlpha(img, cx, cy, r) {
    let m = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            m = Math.max(m, img.data[(y * img.width + x) * 4 + 3]);
        }
    }
    return m;
}

function patchBrightness(img, cx, cy, r) {
    let sum = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? sum / n : 0;
}

// Count pixels whose RGBA differs by more than `tol` in any channel.
function diffCount(a, b, tol) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i]     - b.data[i])     > tol ||
            Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol ||
            Math.abs(a.data[i + 3] - b.data[i + 3]) > tol) n++;
    }
    return n;
}

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU) — skipping skinned mesh test');
} else {
    // Camera on the +Z axis at strip height: world (x,y,0) projects to
    // sx = 128 + x*k, sy = 128 - (y-1)*k with k = 128 / (8 * tan(fov/2)).
    const FOV = 40, EYE = [0, 1, 8];
    scene.setCamera({ fov: FOV, near: 0.1, far: 100, position: EYE, target: [0, 1, 0] });
    function project(x, y, z) {
        const d = EYE[2] - z;
        const k = 128 / (d * Math.tan((FOV / 2) * Math.PI / 180));
        return [Math.round(128 + x * k), Math.round(128 - (y - 1) * k)];
    }

    // Lights: shadow-casting key tilted so the strip's shadow lands on the
    // ground with real area, plus a -Z fill so the camera-facing side is lit.
    const key = scene.createLight({
        type: 'directional', direction: [0, -0.7071, 0.7071],
        color: [1, 1, 1], intensity: 2.5,
    });
    key.castsShadow = true;
    scene.createLight({
        type: 'directional', direction: [0, 0, -1],
        color: [1, 1, 1], intensity: 1.5,
    });

    // Ground plane (shadow receiver).
    scene.createMesh({ mesh: 'plane', halfW: 5, halfD: 5, color: 'white',
                       castsShadow: false, y: 0 });

    // ------------------------------------------------------------------
    // Procedural 2-bone strip geometry
    // ------------------------------------------------------------------
    const ROWS = 9;                       // y = 0, 0.25, ... 2.0
    const positions = new Float32Array(ROWS * 2 * 3);
    const normals   = new Float32Array(ROWS * 2 * 3);
    const boneW = new Float32Array(ROWS * 2 * 4);
    const boneI = new Uint32Array(ROWS * 2 * 4);
    for (let r = 0; r < ROWS; r++) {
        const y = r * 0.25;
        for (let c = 0; c < 2; c++) {
            const v = r * 2 + c;
            positions[v * 3 + 0] = c === 0 ? -0.2 : 0.2;
            positions[v * 3 + 1] = y;
            positions[v * 3 + 2] = 0;
            normals[v * 3 + 2] = 1;
            boneW[v * 4 + 0] = 1;                 // single full-weight bone
            boneI[v * 4 + 0] = y > 1.0 ? 1 : 0;   // rigid split at the hinge
        }
    }
    const indices = new Uint32Array((ROWS - 1) * 6);
    for (let r = 0; r < ROWS - 1; r++) {
        const bl = r * 2, br = r * 2 + 1, tl = (r + 1) * 2, tr = (r + 1) * 2 + 1;
        indices.set([bl, br, tr, bl, tr, tl], r * 6);
    }

    // ------------------------------------------------------------------
    // Reference: static mesh of the same geometry
    // ------------------------------------------------------------------
    const staticNode = scene.createMesh({
        positions, normals, indices, color: 'red', roughness: 0.9,
    });
    const imgStatic = scene.captureFrame();
    assert(imgStatic !== null, 'captureFrame returns image');
    staticNode.visible = false;

    // ------------------------------------------------------------------
    // Skinned node, identity palette == bind pose
    // ------------------------------------------------------------------
    const skin = new SkinData({
        boneWeights: boneW,
        boneIndices: boneI,
        // Identity inverse binds: bone space == model space at rest.
        inverseBindMatrices: new Float32Array([
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
        ]),
        boneCount: 2,
    });

    const node = scene.createSkinnedMesh({
        positions, normals, indices, color: 'red', roughness: 0.9,
        skin,
    });
    assert(node !== null && node !== undefined, 'createSkinnedMesh returns node');
    assert(node.type === 'skinnedMesh', 'node type is skinnedMesh');
    assert(node.boneCount === 2, 'boneCount is 2');
    assert(node.skinReady === true, 'skinReady true');

    const imgIdentity = scene.captureFrame();

    // Identity palette must render like the static mesh: same program family,
    // same geometry — allow AA/precision slack but no structural difference.
    const nDiff = diffCount(imgStatic, imgIdentity, 12);
    assert(nDiff < imgStatic.width * imgStatic.height * 0.01,
        `identity palette matches static mesh (diff pixels ${nDiff})`);

    // Bind pose sanity: strip top present, arm region empty.
    const [txs, tys] = project(0, 1.75, 0);     // top of strip
    const [axs, ays] = project(-0.6, 1, 0);     // where the bent arm will be
    assert(patchMaxAlpha(imgIdentity, txs, tys, 3) > 128,
        'bind pose: strip top visible');
    assert(patchMaxAlpha(imgIdentity, axs, ays, 3) === 0,
        'bind pose: arm region empty');

    // ------------------------------------------------------------------
    // Bend via the real rigging pipeline: Skeleton -> Pose -> palette
    // ------------------------------------------------------------------
    const skel = new Skeleton({ bones: [
        { name: 'root',  parent: -1 },
        { name: 'upper', parent: 0, localT: [0, 1, 0],
          inverseBind: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,0,1] },
    ]});
    const pose = skel.bindPose();
    const pd = pose.data;                 // stride 10: t3 r4(xyzw) s3
    const s45 = Math.SQRT1_2;
    pd[10 + 3] = 0; pd[10 + 4] = 0;       // bone 1 rotation: 90 deg about Z
    pd[10 + 5] = s45; pd[10 + 6] = s45;
    pose.data = pd;
    const mats = pose.computeSkinningMatrices(skel);
    assert(mats.length === 32, 'computeSkinningMatrices yields 2 mat4s');

    // Numeric check against the analytic hinge: M1 = T(0,1,0)*Rz(90)*T(0,-1,0)
    // (column-major): cols = (0,1,0,0)(-1,0,0,0)(0,0,1,0)(1,1,0,1).
    const expected = [0,1,0,0, -1,0,0,0, 0,0,1,0, 1,1,0,1];
    for (let i = 0; i < 16; i++) {
        assert(Math.abs(mats[16 + i] - expected[i]) < 1e-4,
            `skinning matrix [1][${i}] analytic (${mats[16 + i]} vs ${expected[i]})`);
    }

    const staged = node.setSkinningMatrices(mats);
    assert(staged === 2, 'setSkinningMatrices staged 2 matrices');

    const imgBent = scene.captureFrame();

    // Deformation: the top of the strip is gone, the horizontal arm exists.
    assert(patchMaxAlpha(imgBent, txs, tys, 3) === 0,
        'bent: strip top region now empty');
    assert(patchMaxAlpha(imgBent, axs, ays, 3) > 128,
        'bent: horizontal arm visible');
    // Lower half (bone 0) must be untouched by the bend.
    const [lxs, lys] = project(0, 0.5, 0);
    assert(patchMaxAlpha(imgBent, lxs, lys, 3) > 128,
        'bent: lower half still in place');

    // ------------------------------------------------------------------
    // Shadow deformation. Key light travels (0,-1,1)/sqrt2, so a point at
    // height y drops its shadow at z = y: the bent arm (x in [-1,0], y ~= 1)
    // shadows the ground around (-0.6, 0, 1); in bind pose that spot is lit
    // (the vertical strip only shadows x in [-0.2, 0.2]). A mirrored control
    // patch at (+0.6, 0, 1) must stay lit in both.
    // ------------------------------------------------------------------
    const [shx, shy] = project(-0.6, 0, 1);
    const [ctx2, cty2] = project(0.6, 0, 1);
    const litBefore    = patchBrightness(imgIdentity, shx, shy, 3);
    const litAfter     = patchBrightness(imgBent,     shx, shy, 3);
    const controlBefore = patchBrightness(imgIdentity, ctx2, cty2, 3);
    const controlAfter  = patchBrightness(imgBent,     ctx2, cty2, 3);
    assert(litAfter < litBefore * 0.8,
        `bent arm casts deformed shadow (${litBefore.toFixed(1)} -> ${litAfter.toFixed(1)})`);
    assert(Math.abs(controlAfter - controlBefore) < controlBefore * 0.15 + 4,
        `control ground patch unchanged (${controlBefore.toFixed(1)} -> ${controlAfter.toFixed(1)})`);

    // ------------------------------------------------------------------
    // Error paths + introspection on non-skinned nodes
    // ------------------------------------------------------------------
    let threw = false;
    try { scene.createSkinnedMesh({ mesh: 'box' }); } catch (e) { threw = true; }
    assert(threw, 'createSkinnedMesh without skin throws');

    threw = false;
    try {
        scene.createSkinnedMesh({
            mesh: 'box',
            skin: new SkinData({
                boneWeights: new Float32Array(4),
                boneIndices: new Uint32Array(4),
                boneCount: 1,
            }),
        });
    } catch (e) { threw = true; }
    assert(threw, 'createSkinnedMesh with mismatched skin vertex count throws');

    const plainMesh = scene.createMesh({ mesh: 'box', color: 'blue' });
    assert(plainMesh.boneCount === 0, 'plain mesh boneCount 0');
    assert(plainMesh.skinReady === false, 'plain mesh skinReady false');
    threw = false;
    try { plainMesh.setSkinningMatrices(mats); } catch (e) { threw = true; }
    assert(threw, 'setSkinningMatrices on plain mesh throws');

    console.log('skinned mesh tests passed');
}

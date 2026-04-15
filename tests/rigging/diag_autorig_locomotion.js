// diag_autorig_locomotion.js — numerically inspect mesh-forge's rigging
// pipeline without a GUI. Runs the same flow as apps/mesh-forge:
//   loadGLTF → Rig.detectHumanoid → Rig.autoRig → Rig.generateLocomotionCycle
//   → per-frame applySkinning.
//
// The purpose is to tell whether the animated mesh "looks good" purely from
// numbers — specifically to catch the tearing seen in the GUI where triangles
// fly apart during locomotion. Key signals:
//
//   - bind-pose round-trip delta: applying the bind pose through skinning
//     should leave the mesh unchanged. If it doesn't, bone world transforms
//     disagree with the skin's inverseBindMatrices.
//   - skin validation: orphaned verts, bad weight sums, NaNs.
//   - weighted influence per bone: if an arm bone has 0 influence the arms
//     simply won't move.
//   - edge stretch: for every mesh edge, ratio current / bind. Torn triangles
//     produce edges that blow up by 5-50x.
//   - triangle area ratio: complements edge stretch — isolates tears vs.
//     uniform stretches.
//   - per-bone bind-to-anim centroid displacement: shows which bones are
//     actually doing work across the cycle.
//
// Usage:
//   bro-headless tests/test_app tests/rigging/diag_autorig_locomotion.js
//
// Optionally override the input glb via BROFORGE_INPUT env var handling
// would live here; for now the path is hardcoded to a stable MeshyAI file.

'use strict';

const INPUTS = [
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Character_output.glb',
    'D:/moba-game/Meshy_AI_Crimson_Core_Knight_0414114102_generate.glb',
    'D:/moba-game/Meshy_AI_Gilded_Sentinel_0414120138_generate.glb',
    'D:/moba-game/Meshy_AI_Golden_Core_Knight_0414102821_texture.glb',
];
const SPEC  = 'D:/projects/bromesh/data/rig_specs/humanoid.json';
const SAMPLE_TIMES = [0.0, 0.25, 0.5, 0.75];   // fractions of cycle duration
const STRETCH_TEAR_THRESHOLD = 3.0;             // edges stretched >3x = torn

// --- helpers ---------------------------------------------------------------

function summarize(values) {
    if (values.length === 0) return { min:0, max:0, mean:0, p50:0, p95:0, p99:0 };
    const sorted = Array.from(values).sort((a, b) => a - b);
    const n = sorted.length;
    let sum = 0;
    for (const v of sorted) sum += v;
    const pick = (q) => sorted[Math.min(n - 1, Math.max(0, Math.floor(q * n)))];
    return {
        min: sorted[0],
        max: sorted[n - 1],
        mean: sum / n,
        p50: pick(0.5),
        p95: pick(0.95),
        p99: pick(0.99),
    };
}

function fmt(x, d) { return Number(x).toFixed(d == null ? 4 : d); }

function buildUniqueEdges(indices, triCount) {
    // Return unique edge pairs as Uint32Array of [v0, v1, v0, v1, ...].
    const set = new Set();
    const pairs = [];
    for (let t = 0; t < triCount; t++) {
        const a = indices[t * 3 + 0];
        const b = indices[t * 3 + 1];
        const c = indices[t * 3 + 2];
        for (const [u, v] of [[a, b], [b, c], [c, a]]) {
            const lo = Math.min(u, v), hi = Math.max(u, v);
            const key = lo * 16777619 + hi; // cheap mix; collision-tolerant
            if (!set.has(key)) {
                set.add(key);
                pairs.push(lo, hi);
            }
        }
    }
    return pairs;
}

function edgeLengths(pairs, positions, outLens) {
    const count = pairs.length / 2;
    for (let e = 0; e < count; e++) {
        const i = pairs[e * 2 + 0] * 3;
        const j = pairs[e * 2 + 1] * 3;
        const dx = positions[i + 0] - positions[j + 0];
        const dy = positions[i + 1] - positions[j + 1];
        const dz = positions[i + 2] - positions[j + 2];
        outLens[e] = Math.hypot(dx, dy, dz);
    }
}

function triAreas(indices, triCount, positions, out) {
    for (let t = 0; t < triCount; t++) {
        const i0 = indices[t * 3 + 0] * 3;
        const i1 = indices[t * 3 + 1] * 3;
        const i2 = indices[t * 3 + 2] * 3;
        const ax = positions[i1 + 0] - positions[i0 + 0];
        const ay = positions[i1 + 1] - positions[i0 + 1];
        const az = positions[i1 + 2] - positions[i0 + 2];
        const bx = positions[i2 + 0] - positions[i0 + 0];
        const by = positions[i2 + 1] - positions[i0 + 1];
        const bz = positions[i2 + 2] - positions[i0 + 2];
        const cx = ay * bz - az * by;
        const cy = az * bx - ax * bz;
        const cz = ax * by - ay * bx;
        out[t] = 0.5 * Math.hypot(cx, cy, cz);
    }
}

// Dominant-influence histogram: for each vert, take argmax bone, accumulate.
function vertsPerBone(skin, boneCount) {
    const w = skin.boneWeights;
    const b = skin.boneIndices;
    const n = skin.vertexCount;
    const counts = new Uint32Array(boneCount);
    const sumW = new Float32Array(boneCount);
    for (let v = 0; v < n; v++) {
        let bestW = -1, bestB = -1;
        for (let k = 0; k < 4; k++) {
            const wk = w[v * 4 + k];
            const bk = b[v * 4 + k];
            if (wk > bestW) { bestW = wk; bestB = bk; }
            if (bk < boneCount) sumW[bk] += wk;
        }
        if (bestB >= 0 && bestB < boneCount) counts[bestB]++;
    }
    return { counts, sumW };
}

// Per-bone centroid of positions weighted by boneIndex=b influence.
function bonePosCentroid(skin, positions, boneIdx) {
    const w = skin.boneWeights;
    const b = skin.boneIndices;
    const n = skin.vertexCount;
    let sx = 0, sy = 0, sz = 0, sw = 0;
    for (let v = 0; v < n; v++) {
        let weight = 0;
        for (let k = 0; k < 4; k++) {
            if (b[v * 4 + k] === boneIdx) { weight += w[v * 4 + k]; }
        }
        if (weight > 0) {
            sx += weight * positions[v * 3 + 0];
            sy += weight * positions[v * 3 + 1];
            sz += weight * positions[v * 3 + 2];
            sw += weight;
        }
    }
    if (sw <= 0) return null;
    return [sx / sw, sy / sw, sz / sw];
}

// --- main ------------------------------------------------------------------

function runForInput(INPUT) {
    console.log('== loading ' + INPUT);
    const gltf = Mesh.loadGLTF(INPUT);
    const m = gltf.meshes[0];
    if (!m.hasNormals) m.computeNormals();
    console.log('  verts=' + m.vertexCount + ' tris=' + m.triangleCount
        + ' manifold=' + m.isManifold());

    console.log('== detectHumanoid');
    const spec = Rig.specFromFile(SPEC);
    const lms  = Rig.detectHumanoid(m);
    const missing = Rig.missingLandmarks(spec, lms) || [];
    console.log('  spec bones=' + Rig.specBoneCount(spec)
        + ' spec landmarks=' + Rig.specLandmarkCount(spec));
    if (missing.length) console.log('  missing: ' + missing.join(', '));
    else                console.log('  all landmarks detected');

    const forceMethod = globalThis.BROFORGE_METHOD || '';
    console.log('== autoRig (method=' + (forceMethod || 'auto') + ')');
    const t0 = performance.now();
    const rigged = Rig.autoRig(m, spec, lms,
        forceMethod ? { method: forceMethod } : {});
    console.log('  time=' + fmt(performance.now() - t0, 1) + 'ms');
    console.log('  methodUsed=' + rigged.methodUsed);
    if (rigged.warnings && rigged.warnings.length) {
        console.log('  warnings:');
        for (const w of rigged.warnings) console.log('    - ' + w);
    }
    const sk = rigged.skeleton;
    const skin = rigged.skin;
    console.log('  skeleton bones=' + sk.boneCount);
    console.log('  skin verts=' + skin.vertexCount + ' bones=' + skin.boneCount);

    console.log('== SkinData.validate');
    const v = SkinData.validate(m, skin);
    console.log('  orphans=' + v.orphanCount
        + '  badSum=' + v.badSumCount
        + '  nan=' + v.nanCount
        + '  maxInfl=' + v.maxInfluencesObserved
        + '  maxSumDev=' + fmt(v.maxSumDeviation, 6)
        + '  clean=' + v.clean);

    console.log('== weight coverage per bone (dominant influence)');
    const bones = sk.bones;
    const { counts, sumW } = vertsPerBone(skin, sk.boneCount);
    let unusedBones = 0;
    for (let i = 0; i < sk.boneCount; i++) {
        const dom = counts[i];
        const totalW = sumW[i];
        if (totalW === 0) unusedBones++;
        console.log('  [' + String(i).padStart(2) + '] '
            + (bones[i].name || '?').padEnd(28) + ' '
            + 'domVerts=' + String(dom).padStart(5)
            + '  sumW=' + fmt(totalW, 2));
    }
    console.log('  unused bones: ' + unusedBones + '/' + sk.boneCount);

    console.log('== bind-pose round-trip (applySkinning(bindPose) should be identity)');
    const bindPose = sk.bindPose();
    const bindMats = bindPose.computeWorldMatrices(sk);
    const basePositions = new Float32Array(m.positions);
    m.positions = new Float32Array(basePositions);
    m.applySkinning(skin, bindMats);
    {
        let maxD = 0, sumSq = 0;
        const p = m.positions;
        for (let i = 0; i < basePositions.length; i++) {
            const d = p[i] - basePositions[i];
            if (Math.abs(d) > maxD) maxD = Math.abs(d);
            sumSq += d * d;
        }
        console.log('  max delta=' + fmt(maxD, 6)
            + '  rms=' + fmt(Math.sqrt(sumSq / basePositions.length), 6));
    }

    console.log('== generateLocomotionCycle');
    const anim = Rig.generateLocomotionCycle(sk, spec, {});
    console.log('  duration=' + fmt(anim.duration, 3) + 's');

    console.log('== edge & triangle analysis');
    const pairs  = buildUniqueEdges(m.indices, m.triangleCount);
    const eCount = pairs.length / 2;
    const bindLens  = new Float32Array(eCount);
    const curLens   = new Float32Array(eCount);
    const bindAreas = new Float32Array(m.triangleCount);
    const curAreas  = new Float32Array(m.triangleCount);
    // restore bind positions then measure bind edge/area stats
    m.positions = new Float32Array(basePositions);
    edgeLengths(pairs, m.positions, bindLens);
    triAreas(m.indices, m.triangleCount, m.positions, bindAreas);
    const bindEdgeStats = summarize(bindLens);
    const bindAreaStats = summarize(bindAreas);
    console.log('  bind-pose edges: n=' + eCount
        + '  min=' + fmt(bindEdgeStats.min)
        + '  mean=' + fmt(bindEdgeStats.mean)
        + '  p95=' + fmt(bindEdgeStats.p95)
        + '  max=' + fmt(bindEdgeStats.max));
    console.log('  bind-pose tris:  n=' + m.triangleCount
        + '  min=' + fmt(bindAreaStats.min, 6)
        + '  mean=' + fmt(bindAreaStats.mean, 6)
        + '  p95=' + fmt(bindAreaStats.p95, 6)
        + '  max=' + fmt(bindAreaStats.max, 6));

    console.log('== animated-frame deformation');
    const stretchBins = { '≤1.1':0, '≤1.5':0, '≤2':0, '≤3':0, '≤5':0, '>5':0 };
    function binStretch(r) {
        if (r <= 1.1) stretchBins['≤1.1']++;
        else if (r <= 1.5) stretchBins['≤1.5']++;
        else if (r <= 2)   stretchBins['≤2']++;
        else if (r <= 3)   stretchBins['≤3']++;
        else if (r <= 5)   stretchBins['≤5']++;
        else               stretchBins['>5']++;
    }

    for (const frac of SAMPLE_TIMES) {
        const t = frac * anim.duration;
        // reset mesh to bind, evaluate anim pose, skin.
        m.positions = new Float32Array(basePositions);
        const pose = anim.evaluate(sk, t, { loop: true });
        const mats = pose.computeWorldMatrices(sk);
        m.applySkinning(skin, mats);

        edgeLengths(pairs, m.positions, curLens);
        triAreas(m.indices, m.triangleCount, m.positions, curAreas);

        // per-edge stretch ratio
        const stretch = new Float32Array(eCount);
        for (let e = 0; e < eCount; e++) {
            const b = bindLens[e];
            stretch[e] = b > 1e-9 ? curLens[e] / b : 1;
        }
        const stretchStats = summarize(stretch);
        let torn = 0;
        for (let e = 0; e < eCount; e++) if (stretch[e] > STRETCH_TEAR_THRESHOLD) torn++;

        // reset bin counts for this frame
        for (const k of Object.keys(stretchBins)) stretchBins[k] = 0;
        for (let e = 0; e < eCount; e++) binStretch(stretch[e]);

        // triangle area ratio
        const areaRatio = new Float32Array(m.triangleCount);
        for (let i = 0; i < m.triangleCount; i++) {
            const b = bindAreas[i];
            areaRatio[i] = b > 1e-12 ? curAreas[i] / b : 1;
        }
        const areaStats = summarize(areaRatio);

        console.log('  t=' + fmt(frac, 2) + '*dur:');
        console.log('    edge stretch:  min=' + fmt(stretchStats.min)
            + '  p50=' + fmt(stretchStats.p50)
            + '  p95=' + fmt(stretchStats.p95)
            + '  p99=' + fmt(stretchStats.p99)
            + '  max=' + fmt(stretchStats.max));
        console.log('    edge bins: '
            + Object.entries(stretchBins)
                .map(([k, v]) => k + '=' + v).join('  '));
        console.log('    torn edges (>' + STRETCH_TEAR_THRESHOLD + 'x): ' + torn
            + ' / ' + eCount
            + '  (' + fmt(100 * torn / eCount, 2) + '%)');
        console.log('    tri area ratio: min=' + fmt(areaStats.min)
            + '  p50=' + fmt(areaStats.p50)
            + '  p95=' + fmt(areaStats.p95)
            + '  max=' + fmt(areaStats.max));
    }

    console.log('== per-bone centroid displacement (bind → mid-anim)');
    m.positions = new Float32Array(basePositions);
    const midPose = anim.evaluate(sk, anim.duration * 0.5, { loop: true });
    const midMats = midPose.computeWorldMatrices(sk);
    m.applySkinning(skin, midMats);
    const midPositions = new Float32Array(m.positions);
    let moved = 0, still = 0;
    for (let i = 0; i < sk.boneCount; i++) {
        const cBind = bonePosCentroid(skin, basePositions, i);
        const cAnim = bonePosCentroid(skin, midPositions, i);
        if (!cBind || !cAnim) continue;
        const d = Math.hypot(
            cBind[0] - cAnim[0],
            cBind[1] - cAnim[1],
            cBind[2] - cAnim[2]);
        if (d < 1e-4) still++; else moved++;
        console.log('  [' + String(i).padStart(2) + '] '
            + (bones[i].name || '?').padEnd(28) + '  disp=' + fmt(d, 5));
    }
    console.log('  moved=' + moved + '  still=' + still);

    console.log('== done');
}

for (const input of INPUTS) {
    console.log('');
    console.log('############################################################');
    console.log('# ' + input.split('/').pop());
    console.log('############################################################');
    try {
        runForInput(input);
    } catch (e) {
        console.log('ERROR: ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}

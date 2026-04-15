// SkinData — construction, properties, normalize, validate, transfer, applySkinning.

// ── Construct from typed arrays ──────────────────────────────────────────────
{
    const skin = new SkinData({
        boneWeights:         new Float32Array([1, 0, 0, 0,  0.5, 0.5, 0, 0]),
        boneIndices:         new Uint32Array ([0, 0, 0, 0,  0,   1,   0, 0]),
        inverseBindMatrices: new Float32Array(16 * 2),
        boneCount: 2,
    });
    assert(skin.boneCount === 2,            'boneCount preserved');
    assert(skin.vertexCount === 2,          'vertexCount derived from weights');
    assert(skin.boneWeights.length === 8,   'boneWeights round-trips');
    assert(skin.boneIndices.length === 8,   'boneIndices round-trips');
    assert(skin.inverseBindMatrices.length === 32, 'inverseBindMatrices round-trips');
}

// ── boneCount inferred from inverseBindMatrices when not specified ──────────
{
    const skin = new SkinData({ inverseBindMatrices: new Float32Array(16 * 3) });
    assert(skin.boneCount === 3, 'boneCount inferred from IBM length');
}

// ── normalize() collapses garbage weights to sum=1 ──────────────────────────
{
    const skin = new SkinData({
        boneWeights: new Float32Array([2, 2, 0, 0,  0.1, 0.1, 0, 0]),
        boneIndices: new Uint32Array ([0, 1, 0, 0,  0,   1,   0, 0]),
        boneCount: 2,
    });
    skin.normalize();
    const w = skin.boneWeights;
    const sum0 = w[0] + w[1] + w[2] + w[3];
    const sum1 = w[4] + w[5] + w[6] + w[7];
    assert(Math.abs(sum0 - 1) < 1e-4, 'vertex 0 normalized: ' + sum0);
    assert(Math.abs(sum1 - 1) < 1e-4, 'vertex 1 normalized: ' + sum1);
}

// ── validate() reports clean state ──────────────────────────────────────────
{
    const mesh = new Mesh({
        positions: new Float32Array([0,0,0, 1,0,0]),
        indices:   new Uint32Array([]),
    });
    const skin = new SkinData({
        boneWeights: new Float32Array([1, 0, 0, 0,  0.5, 0.5, 0, 0]),
        boneIndices: new Uint32Array ([0, 0, 0, 0,  0,   1,   0, 0]),
        boneCount: 2,
    });
    const v = SkinData.validate(mesh, skin);
    assert(v.vertexCount === 2,          'validate.vertexCount');
    assert(v.orphanCount === 0,          'no orphans');
    assert(v.badSumCount === 0,          'all sums OK');
    assert(v.nanCount === 0,             'no NaN');
    assert(v.clean === true,             'clean');
    assert(v.maxInfluencesObserved >= 1, 'observed at least one influence');
}

// ── validate() flags an orphan vertex ───────────────────────────────────────
{
    const mesh = new Mesh({ positions: new Float32Array([0,0,0, 1,0,0]) });
    const skin = new SkinData({
        boneWeights: new Float32Array([1, 0, 0, 0,  0, 0, 0, 0]),
        boneIndices: new Uint32Array ([0, 0, 0, 0,  0, 0, 0, 0]),
        boneCount: 1,
    });
    const v = SkinData.validate(mesh, skin);
    assert(v.orphanCount === 1, 'orphan detected');
    assert(v.clean === false,   'not clean');
}

// ── applySkinning with identity pose leaves positions unchanged ─────────────
{
    const m = Mesh.box(0.5, 0.5, 0.5);
    const before = new Float32Array(m.positions);
    const vc = m.vertexCount;
    // 1 bone, identity inverse-bind, all weight on bone 0
    const wts = new Float32Array(vc * 4);
    const idx = new Uint32Array(vc * 4);
    for (let i = 0; i < vc; i++) wts[i * 4] = 1.0;
    const ibm = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
    const skin = new SkinData({ boneWeights: wts, boneIndices: idx, inverseBindMatrices: ibm, boneCount: 1 });
    const pose = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
    m.applySkinning(skin, pose);
    const after = m.positions;
    let maxDelta = 0;
    for (let i = 0; i < before.length; i++) maxDelta = Math.max(maxDelta, Math.abs(before[i] - after[i]));
    assert(maxDelta < 1e-4, 'identity skinning is no-op (max delta ' + maxDelta + ')');
}

// ── applySkinning with a translation pose moves positions by that translation ──
{
    const m = Mesh.box(0.5, 0.5, 0.5);
    const before = new Float32Array(m.positions);
    const vc = m.vertexCount;
    const wts = new Float32Array(vc * 4);
    const idx = new Uint32Array(vc * 4);
    for (let i = 0; i < vc; i++) wts[i * 4] = 1.0;
    const ibm = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
    const skin = new SkinData({ boneWeights: wts, boneIndices: idx, inverseBindMatrices: ibm, boneCount: 1 });
    // column-major mat4 with translation (2, 3, 4)
    const pose = new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        2, 3, 4, 1,
    ]);
    m.applySkinning(skin, pose);
    const after = m.positions;
    for (let i = 0; i < vc; i++) {
        assert(Math.abs(after[i*3]   - (before[i*3]   + 2)) < 1e-4, 'x translated');
        assert(Math.abs(after[i*3+1] - (before[i*3+1] + 3)) < 1e-4, 'y translated');
        assert(Math.abs(after[i*3+2] - (before[i*3+2] + 4)) < 1e-4, 'z translated');
    }
}

// ── SkinData.transfer projects weights from source -> target ────────────────
{
    const src = Mesh.box(0.5, 0.5, 0.5);
    const vc  = src.vertexCount;
    const wts = new Float32Array(vc * 4);
    const idx = new Uint32Array(vc * 4);
    for (let i = 0; i < vc; i++) wts[i * 4] = 1.0;
    const srcSkin = new SkinData({
        boneWeights: wts, boneIndices: idx,
        inverseBindMatrices: new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]),
        boneCount: 1,
    });
    const dst = Mesh.box(0.5, 0.5, 0.5);
    const dstSkin = SkinData.transfer(dst, src, srcSkin);
    assert(dstSkin.vertexCount === dst.vertexCount, 'transferred vertexCount matches target');
    // each target vertex should have at least one non-zero weight
    const dw = dstSkin.boneWeights;
    let hits = 0;
    for (let i = 0; i < dst.vertexCount; i++) {
        const s = dw[i*4] + dw[i*4+1] + dw[i*4+2] + dw[i*4+3];
        if (s > 0) hits++;
    }
    assert(hits === dst.vertexCount, 'all transferred verts have weight');
}

console.log('PASS test_skin');

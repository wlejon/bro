// Inspect the rigged Stone Sentinel GLBs from Meshy.

const candidates = [
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Character_output.glb',
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Animation_Walking_withSkin.glb',
    'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Animation_Running_withSkin.glb',
];

for (const path of candidates) {
    console.log('========================================');
    console.log(path.split('/').pop());
    console.log('========================================');

    const scene = Mesh.loadGLTF(path);
    console.log('meshes=' + scene.meshes.length
        + ' skins=' + scene.skins.length
        + ' skeletons=' + scene.skeletons.length
        + ' animations=' + scene.animations.length);

    if (scene.meshes.length === 0) { console.log(''); continue; }

    for (let i = 0; i < scene.meshes.length; i++) {
        const m = scene.meshes[i];
        console.log('  mesh[' + i + ']: verts=' + m.vertexCount
            + ' tris=' + m.triangleCount
            + ' hasN=' + m.hasNormals + ' hasUV=' + m.hasUVs);
    }

    for (let i = 0; i < scene.skeletons.length; i++) {
        const s = scene.skeletons[i];
        console.log('  skeleton[' + i + ']: bones=' + s.boneCount + ' sockets=' + s.socketCount);
        const bones = s.bones;
        const limit = Math.min(bones.length, 8);
        for (let j = 0; j < limit; j++) {
            const b = bones[j];
            console.log('    [' + j + '] ' + b.name + ' parent=' + b.parent
                + ' T=[' + b.localT.map(x => x.toFixed(3)).join(',') + ']');
        }
        if (bones.length > limit) console.log('    ... +' + (bones.length - limit) + ' more');
    }

    for (let i = 0; i < scene.skins.length; i++) {
        const sk = scene.skins[i];
        console.log('  skin[' + i + ']: verts=' + sk.vertexCount + ' bones=' + sk.boneCount
            + ' weightsLen=' + sk.boneWeights.length + ' indicesLen=' + sk.boneIndices.length);
    }

    for (let i = 0; i < scene.animations.length; i++) {
        const a = scene.animations[i];
        console.log('  animation[' + i + ']: name="' + a.name + '" dur=' + a.duration.toFixed(3)
            + ' channels=' + a.channelCount);
        const chs = a.channels;
        const byPath = { translation: 0, rotation: 0, scale: 0 };
        const bones = new Set();
        for (const c of chs) { byPath[c.path] = (byPath[c.path]||0) + 1; bones.add(c.boneIndex); }
        console.log('    by-path: T=' + byPath.translation + ' R=' + byPath.rotation + ' S=' + byPath.scale
            + ' bonesAffected=' + bones.size);
        if (chs.length) {
            const c0 = chs[0];
            console.log('    sample channel[0]: bone=' + c0.boneIndex + ' ' + c0.path
                + '/' + c0.interp + ' keys=' + c0.times.length
                + ' t=[' + c0.times[0].toFixed(3) + '..'
                + c0.times[c0.times.length-1].toFixed(3) + ']');
        }
    }

    // End-to-end skinning sanity: build skinning matrices at bind pose, apply to mesh.
    if (scene.skins.length && scene.skeletons.length && scene.skins[0].boneCount > 0) {
        const m = scene.meshes[0];
        const sk = scene.skins[0];
        const skel = scene.skeletons[0];
        const v = SkinData.validate(m, sk);
        console.log('  skin validate: clean=' + v.clean
            + ' orphans=' + v.orphanCount
            + ' badSum=' + v.badSumCount
            + ' nan=' + v.nanCount
            + ' maxSumDev=' + v.maxSumDeviation.toFixed(5)
            + ' maxInfl=' + v.maxInfluencesObserved);

        const pose = skel.bindPose();
        const matrices = pose.computeSkinningMatrices(skel);
        const before = new Float32Array(m.positions);
        m.applySkinning(sk, matrices);
        const after = m.positions;
        let maxDelta = 0, sumSq = 0;
        for (let i = 0; i < before.length; i++) {
            const d = before[i] - after[i];
            if (Math.abs(d) > maxDelta) maxDelta = Math.abs(d);
            sumSq += d * d;
        }
        console.log('  bind-pose skinning delta: max=' + maxDelta.toFixed(5)
            + ' rms=' + Math.sqrt(sumSq / before.length).toFixed(5));

        // If animations are present, sample mid-cycle and re-skin a fresh copy.
        if (scene.animations.length > 0) {
            const anim = scene.animations[0];
            const t = anim.duration * 0.5;
            const animPose = anim.evaluate(skel, t, { loop: false });
            const animMats = animPose.computeSkinningMatrices(skel);
            const m2 = new Mesh({
                positions: before,
                indices:   m.indices,
                normals:   m.normals,
                uvs:       m.uvs,
            });
            m2.applySkinning(sk, animMats);
            const after2 = m2.positions;
            let maxAnim = 0, sumAnimSq = 0;
            for (let i = 0; i < before.length; i++) {
                const d = before[i] - after2[i];
                if (Math.abs(d) > maxAnim) maxAnim = Math.abs(d);
                sumAnimSq += d * d;
            }
            console.log('  mid-anim skinning delta vs bind: max=' + maxAnim.toFixed(5)
                + ' rms=' + Math.sqrt(sumAnimSq / before.length).toFixed(5));
        }
    }

    console.log('');
}

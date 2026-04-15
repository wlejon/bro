// Rig stage - detect landmarks, autoRig, visualize skeleton.

(function() {
    const RIG_SPECS_DIR = 'D:/projects/bromesh/data/rig_specs';

    function inputMesh(ps) { return ps.texturedMesh || ps.cleanMesh || ps.sourceMesh; }

    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <label>Rig spec</label>
            <select id="rig-spec">
                <option value="humanoid">humanoid</option>
                <option value="quadruped">quadruped</option>
                <option value="hexapod">hexapod</option>
                <option value="octopod">octopod</option>
            </select>
            <button id="run-detect">1. Detect landmarks</button>
            <div class="readout" id="lm-out">landmarks not yet detected</div>
            <button id="run-rig" disabled>2. Auto-rig</button>
            <div class="readout" id="rig-out">not rigged</div>
        `;
        const lmOut = document.getElementById('lm-out');
        const rigOut = document.getElementById('rig-out');
        const runRig = document.getElementById('run-rig');
        const specSel = document.getElementById('rig-spec');

        document.getElementById('run-detect').addEventListener('click', () => {
            const m = inputMesh(ps);
            if (!m) { ctx.setStatus('no mesh', true); return; }
            const specName = specSel.value;
            try {
                ps.spec = Rig.specFromFile(RIG_SPECS_DIR + '/' + specName + '.json');
                if (specName === 'humanoid') {
                    ps.landmarks = Rig.detectHumanoid(m);
                } else if (specName === 'quadruped') {
                    ps.landmarks = Rig.detectQuadruped(m);
                } else {
                    throw new Error('no detector for ' + specName + ' - manual landmarks required');
                }
                ps.missing = Rig.missingLandmarks(ps.spec, ps.landmarks) || [];
                const parts = ['spec: ' + specName];
                parts.push('bones: ' + Rig.specBoneCount(ps.spec));
                parts.push('spec landmarks: ' + Rig.specLandmarkCount(ps.spec));
                if (ps.missing.length > 0) {
                    parts.push('missing (' + ps.missing.length + '): ' + ps.missing.join(', '));
                } else {
                    parts.push('all landmarks detected');
                }
                lmOut.textContent = parts.join('\n');
                runRig.disabled = false;
                ctx.setStatus('landmarks detected');
            } catch(e) {
                ctx.setStatus('detect failed: ' + e.message, true);
            }
        });

        runRig.addEventListener('click', () => {
            const m = inputMesh(ps);
            if (!m || !ps.spec || !ps.landmarks) return;
            ctx.setStatus('auto-rigging (this may take a few seconds)...');
            try {
                const result = Rig.autoRig(m, ps.spec, ps.landmarks, {});
                ps.skeleton = result.skeleton;
                ps.skin = result.skin;

                // Validate the skin (orphan verts, bad weight sums, NaNs).
                const v = SkinData.validate(m, result.skin);

                // Bind-pose sanity: applying the skeleton's bind pose should
                // leave the mesh positions unchanged. If maxDelta is large
                // then bind-pose world transforms disagree with the skin's
                // inverseBindMatrices — autoRig's bone placement and the
                // baked invBind are inconsistent and nothing downstream will
                // work right.
                const bindPose = ps.skeleton.bindPose();
                const bindMats = bindPose.computeWorldMatrices(ps.skeleton);
                const before = new Float32Array(m.positions);
                m.applySkinning(ps.skin, bindMats);
                const after = m.positions;
                let maxD = 0, sumSq = 0;
                for (let i = 0; i < before.length; i++) {
                    const d = before[i] - after[i];
                    if (Math.abs(d) > maxD) maxD = Math.abs(d);
                    sumSq += d * d;
                }
                // Restore bind-pose positions so downstream stages see the
                // mesh unchanged.
                m.positions = before;
                m.computeNormals();

                rigOut.textContent =
                    'skeleton bones: ' + (result.skeleton?.boneCount ?? '?') + '\n' +
                    'skin verts: ' + (result.skin?.vertexCount ?? '?') + '\n' +
                    'orphans: ' + v.orphanCount + '  badSum: ' + v.badSumCount +
                    '  nan: ' + v.nanCount + '\n' +
                    'maxInfluences: ' + v.maxInfluencesObserved +
                    '  maxSumDev: ' + v.maxSumDeviation.toFixed(4) + '\n' +
                    'bind-pose delta max: ' + maxD.toFixed(4) +
                    '  rms: ' + Math.sqrt(sumSq / before.length).toFixed(4) + '\n' +
                    'missing: ' + ((result.missingLandmarks || []).join(', ') || 'none');
                ctx.setStatus('rig done');
                ctx.rebuildStageList();
            } catch(e) {
                ctx.setStatus('autoRig failed: ' + e.message, true);
            }
        });
    }

    window.Stages.push({
        id: 'rig',
        label: '6. Rig',
        canEnter: (ps) => !!(ps.cleanMesh || ps.sourceMesh),
        mount,
    });
})();

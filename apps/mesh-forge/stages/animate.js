// Animate stage - generate locomotion, CPU-skin each frame.

(function() {
    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <button id="gen-loco">Generate locomotion cycle</button>
            <div class="readout" id="an-out">no animation</div>
            <label>time</label>
            <input type="range" id="an-time" min="0" max="1" step="0.001" value="0">
            <button id="an-play">Play / pause</button>
        `;
        const out = document.getElementById('an-out');
        const tSlider = document.getElementById('an-time');
        const playBtn = document.getElementById('an-play');

        const m = ps.cleanMesh || ps.sourceMesh;
        if (m) {
            // Snapshot bind positions so we can restore per frame before applySkinning.
            ps.cleanBase = {
                positions: new Float32Array(m.positions),
                normals: m.hasNormals ? new Float32Array(m.normals) : null,
            };
        }

        document.getElementById('gen-loco').addEventListener('click', () => {
            if (!ps.skeleton || !ps.spec) { ctx.setStatus('no skeleton', true); return; }
            try {
                ps.activeAnim = Rig.generateLocomotionCycle(ps.skeleton, ps.spec, {});
                ps.animTime = 0;
                ps.animPlaying = true;
                out.textContent =
                    'duration: ' + (ps.activeAnim?.duration?.toFixed?.(2) ?? '?') + 's';
                ctx.setStatus('locomotion generated');
            } catch(e) {
                ctx.setStatus('locomotion failed: ' + e.message, true);
            }
        });

        playBtn.addEventListener('click', () => {
            ps.animPlaying = !ps.animPlaying;
        });

        tSlider.addEventListener('input', (e) => {
            ps.animPlaying = false;
            const dur = ps.activeAnim?.duration || 1;
            ps.animTime = parseFloat(e.target.value) * dur;
        });
    }

    function tick(ps, ctx, dtMs) {
        if (!ps.activeAnim || !ps.skeleton || !ps.skin) return;
        const m = ps.cleanMesh || ps.sourceMesh;
        if (!m || !ps.cleanBase) return;

        if (ps.animPlaying) ps.animTime += dtMs * 0.001;
        const dur = ps.activeAnim.duration || 1;
        const t = ((ps.animTime % dur) + dur) % dur;

        try {
            const pose = ps.activeAnim.evaluate(ps.skeleton, t, { loop: true });
            // applySkinning multiplies pose*invBind internally (see
            // bromesh/src/manipulation/skin.cpp), so pass WORLD matrices —
            // passing computeSkinningMatrices here causes a double-inverse
            // and the mesh collapses toward origin.
            const mats = pose.computeWorldMatrices(ps.skeleton);

            // Restore bind pose positions before each skinning application.
            m.positions = new Float32Array(ps.cleanBase.positions);
            if (ps.cleanBase.normals) m.normals = new Float32Array(ps.cleanBase.normals);

            m.applySkinning(ps.skin, mats);
            m.computeNormals();
            if (ps.sceneNodes.base) ps.sceneNodes.base.updateMesh(m);

            const tSlider = document.getElementById('an-time');
            if (tSlider && !tSlider.matches(':active')) {
                tSlider.value = String(t / dur);
            }
        } catch(e) {
            ctx.setStatus('animate tick: ' + e.message, true);
            ps.animPlaying = false;
        }
    }

    window.Stages.push({
        id: 'animate',
        label: '7. Animate',
        canEnter: (ps) => !!(ps.skeleton && ps.skin),
        mount,
        tick,
    });
})();

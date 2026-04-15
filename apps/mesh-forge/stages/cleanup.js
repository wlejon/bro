// Cleanup stage - weld, remesh, smooth, simplify. Stub; wire in next pass.

(function() {
    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <div class="readout">source verts: ${ps.sourceMesh?.vertexCount ?? '-'}\nsource tris: ${ps.sourceMesh?.triangleCount ?? '-'}</div>
            <label>weld epsilon</label>
            <input type="number" id="weld-eps" value="0.00001" step="0.00001">
            <label>isotropic edge length (auto if blank)</label>
            <input type="number" id="edge-len" value="" step="0.001">
            <label>Taubin smooth iterations</label>
            <input type="number" id="smooth-iters" value="10" min="0" max="100" step="1">
            <label>simplify target tri count</label>
            <input type="number" id="simplify-target" value="10000" min="100" step="100">
            <button id="run-cleanup">Run cleanup</button>
            <div class="readout" id="cleanup-out">not run yet</div>
        `;

        const out = document.getElementById('cleanup-out');

        document.getElementById('run-cleanup').addEventListener('click', () => {
            if (!ps.sourceMesh) {
                ctx.setStatus('no source mesh', true);
                return;
            }
            ctx.setStatus('cleanup: working...');
            const src = ps.sourceMesh;
            const eps = parseFloat(document.getElementById('weld-eps').value);
            const edgeRaw = document.getElementById('edge-len').value;
            const smoothIters = parseInt(document.getElementById('smooth-iters').value, 10) || 0;
            const target = parseInt(document.getElementById('simplify-target').value, 10) || src.triangleCount;

            // We rebuild cleanMesh from source each run so params iterate cleanly.
            const bb = src.computeBBox();
            const diag = Math.hypot(bb.max[0]-bb.min[0], bb.max[1]-bb.min[1], bb.max[2]-bb.min[2]);
            const edgeLen = edgeRaw ? parseFloat(edgeRaw) : (diag / 200);

            let m = src.clone ? src.clone() : src; // fall through if no clone; TODO

            const steps = [];
            try {
                if (eps > 0 && m.weld) { m.weld(eps); steps.push(`weld ${eps}`); }
            } catch(e) { ctx.setStatus('weld: ' + e.message, true); return; }
            try {
                if (m.remeshIsotropic) { m.remeshIsotropic(edgeLen, 3); steps.push(`remesh ${edgeLen.toFixed(4)}`); }
            } catch(e) { ctx.setStatus('remesh: ' + e.message, true); return; }
            try {
                if (smoothIters > 0 && m.smoothTaubin) {
                    m.smoothTaubin(0.5, -0.53, smoothIters);
                    steps.push(`smoothTaubin ${smoothIters}`);
                }
            } catch(e) { ctx.setStatus('smooth: ' + e.message, true); return; }
            try {
                if (target > 0 && target < m.triangleCount && m.simplifyToTriangleCount) {
                    m.simplifyToTriangleCount(target, 0.1);
                    steps.push(`simplify->${target}`);
                }
            } catch(e) { ctx.setStatus('simplify: ' + e.message, true); return; }

            try { m.computeNormals(); } catch(_) {}

            ps.cleanMesh = m;
            ctx.invalidateDownstream('cleanup');

            // Replace scene node so we render the cleaned mesh.
            if (ps.sceneNodes.base) { try { ps.sceneNodes.base.destroy(); } catch(_) {} }
            ps.sceneNodes.base = scene.createMesh({
                data: m,
                color: [0.80, 0.80, 0.85],
                name: 'clean',
            });

            out.textContent =
                'steps: ' + steps.join(' -> ') + '\n' +
                'verts: ' + m.vertexCount + '\n' +
                'tris: ' + m.triangleCount;
            ctx.setStatus('cleanup done');
            ctx.rebuildStageList();
        });
    }

    window.Stages.push({
        id: 'cleanup',
        label: '2. Cleanup',
        canEnter: (ps) => !!ps.sourceMesh,
        mount,
    });
})();

// UV unwrap stage - stub until chartColors viz lands.

(function() {
    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <div class="readout">input verts: ${ps.cleanMesh?.vertexCount ?? '-'}</div>
            <button id="run-uv">Run unwrap</button>
            <div class="readout" id="uv-out">not run yet</div>
        `;
        const out = document.getElementById('uv-out');
        document.getElementById('run-uv').addEventListener('click', () => {
            if (!ps.cleanMesh) { ctx.setStatus('no clean mesh', true); return; }
            ctx.setStatus('unwrapping...');
            try {
                ps.uvResult = ps.cleanMesh.unwrapUVs();
                out.textContent =
                    'charts: ' + (ps.uvResult?.charts?.length ?? '?') + '\n' +
                    'uv floats: ' + (ps.uvResult?.uvs?.length ?? '?');
                ctx.setStatus('unwrap done');
                ctx.rebuildStageList();
            } catch(e) {
                ctx.setStatus('unwrap failed: ' + e.message, true);
            }
        });
    }
    window.Stages.push({
        id: 'uv',
        label: '3. UV unwrap',
        canEnter: (ps) => !!ps.cleanMesh,
        mount,
    });
})();

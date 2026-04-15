// Export untextured - save cleaned+unwrapped mesh as .glb for MeshyAI texturing.

(function() {
    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <div class="readout">verts: ${ps.cleanMesh?.vertexCount ?? '-'}\ntris: ${ps.cleanMesh?.triangleCount ?? '-'}\nuvs: ${ps.uvResult ? 'yes' : 'no'}</div>
            <p style="color:#888;font-size:11px">Upload the saved .glb to MeshyAI, choose "Texture existing model", download the textured return, then proceed to the Reimport stage.</p>
            <button id="run-export">Save for texturing...</button>
            <div class="readout" id="ex-out">not saved</div>
        `;
        const out = document.getElementById('ex-out');
        document.getElementById('run-export').addEventListener('click', () => {
            if (!ps.cleanMesh) { ctx.setStatus('no mesh', true); return; }
            const path = showSaveFileDialog('glTF|glb', 'forge_untextured.glb');
            if (!path) return;
            try {
                ps.cleanMesh.saveGLTF(path, {});
                ps.untexturedExportPath = path;
                out.textContent = 'saved: ' + path;
                ctx.setStatus('export ok');
            } catch(e) {
                ctx.setStatus('export failed: ' + e.message, true);
            }
        });
    }
    window.Stages.push({
        id: 'export_untextured',
        label: '4. Export for texturing',
        canEnter: (ps) => !!ps.cleanMesh,
        mount,
    });
})();

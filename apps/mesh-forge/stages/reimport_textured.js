// Reimport textured - load MeshyAI's textured return, apply to current topology.

(function() {
    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <button id="run-reimport">Open textured glTF...</button>
            <div class="readout" id="rt-out">not reimported</div>
        `;
        const out = document.getElementById('rt-out');
        document.getElementById('run-reimport').addEventListener('click', () => {
            const r = showOpenFileDialog('glTF|glb;gltf', false);
            if (!Array.isArray(r) || r.length === 0) return;
            try {
                const g = Mesh.loadGLTF(r[0]);
                if (!g.meshes || g.meshes.length === 0) throw new Error('no meshes');
                const m = g.meshes[0];
                const img = (g.images && g.images[0]) ? g.images[0] : null;

                const lines = [
                    'loaded: ' + r[0].split('/').pop(),
                    'verts: ' + m.vertexCount + '  (clean=' + (ps.cleanMesh?.vertexCount ?? '?') + ')',
                    'tris: ' + m.triangleCount + '  (clean=' + (ps.cleanMesh?.triangleCount ?? '?') + ')',
                    'image: ' + (img ? (img.width + 'x' + img.height) : 'none'),
                ];
                out.textContent = lines.join('\n');

                // Replace scene node so user can see the texture applied.
                if (ps.sceneNodes.base) { try { ps.sceneNodes.base.destroy(); } catch(_) {} }
                const opts = { data: m, name: 'textured' };
                if (img && img.data) {
                    opts.texture = { width: img.width, height: img.height, data: img.data };
                    opts.color = [1, 1, 1, 1];
                }
                ps.sceneNodes.base = scene.createMesh(opts);
                ps.texturedMesh = m;
                ctx.setStatus('reimport ok');
                ctx.rebuildStageList();
            } catch(e) {
                ctx.setStatus('reimport failed: ' + e.message, true);
            }
        });
    }
    window.Stages.push({
        id: 'reimport_textured',
        label: '5. Reimport textured',
        canEnter: (ps) => !!ps.cleanMesh,
        mount,
    });
})();

// Load stage - pick a .glb via native file dialog, loadGLTF, show in scene.

(function() {
    function mount(ps, ctx) {
        ctx.paramsBody.innerHTML = `
            <button id="open-btn">Open glTF...</button>
            <div class="readout" id="load-readout">no file loaded</div>
            <label>Recent</label>
            <select id="recent-select">
                <option value="">-</option>
            </select>
        `;

        const readout = document.getElementById('load-readout');
        const recent = document.getElementById('recent-select');

        // Populate with a couple of known MeshyAI demo paths so the user can
        // pick them without browsing - but showOpenFileDialog is the primary path.
        const samples = [
            'D:/moba-game/Meshy_AI_Stone_Sentinel_biped_Character_output.glb',
            'D:/moba-game/Meshy_AI_Crimson_Core_Knight_0414114102_generate.glb',
            'D:/moba-game/Meshy_AI_Gilded_Sentinel_0414120138_generate.glb',
            'D:/moba-game/Meshy_AI_Golden_Core_Knight_0414102821_texture.glb',
        ];
        for (const p of samples) {
            const o = document.createElement('option');
            o.value = p;
            o.textContent = p.split('/').pop();
            recent.appendChild(o);
        }

        function refreshReadout() {
            if (!ps.sourceMesh) {
                readout.textContent = 'no file loaded';
                return;
            }
            const g = ps.sourceGltf || {};
            readout.textContent =
                'file: ' + (ps.sourcePath || '').split('/').pop() + '\n' +
                'meshes: ' + (g.meshes ? g.meshes.length : 0) + '\n' +
                'verts: ' + ps.sourceMesh.vertexCount + '\n' +
                'tris: ' + ps.sourceMesh.triangleCount + '\n' +
                'skins: ' + (g.skins ? g.skins.length : 0) + '\n' +
                'skeletons: ' + (g.skeletons ? g.skeletons.length : 0) + '\n' +
                'animations: ' + (g.animations ? g.animations.length : 0);
        }

        function loadPath(path) {
            if (!path) return;
            ctx.setStatus('loading ' + path.split('/').pop() + '...');
            try {
                const g = Mesh.loadGLTF(path);
                if (!g || !g.meshes || g.meshes.length === 0) {
                    throw new Error('no meshes in file');
                }
                // Merge all primitives into a single mesh for the pipeline.
                // MeshyAI glbs are typically single-primitive; if not, we only
                // take meshes[0] for now and surface a warning.
                if (g.meshes.length > 1) {
                    ctx.setStatus('warning: ' + g.meshes.length + ' meshes, using first', false);
                }
                const m = g.meshes[0];
                if (!m.hasNormals) m.computeNormals();

                // Tear down previous load, invalidate pipeline
                ctx.invalidateDownstream('load');
                if (ps.sceneNodes.base) {
                    try { ps.sceneNodes.base.destroy(); } catch(_) {}
                    ps.sceneNodes.base = null;
                }

                ps.sourceMesh = m;
                ps.sourcePath = path;
                ps.sourceGltf = g;

                ps.sceneNodes.base = scene.createMesh({
                    data: m,
                    color: [0.80, 0.80, 0.85],
                    name: 'source',
                });

                fitCameraToMesh(m);
                refreshReadout();
                ctx.rebuildStageList();
                ctx.setStatus('loaded ' + path.split('/').pop());
            } catch (e) {
                ctx.setStatus('load failed: ' + e.message, true);
            }
        }

        document.getElementById('open-btn').addEventListener('click', () => {
            const r = showOpenFileDialog('glTF|glb;gltf', false);
            // Dialog returns an array of paths (empty if cancelled).
            if (Array.isArray(r) && r.length > 0) loadPath(r[0]);
        });

        recent.addEventListener('change', (e) => {
            if (e.target.value) loadPath(e.target.value);
        });

        refreshReadout();
    }

    function unmount(ps, ctx) { /* keep base node alive */ }

    window.Stages.push({
        id: 'load',
        label: '1. Load',
        canEnter: () => true,
        mount,
        unmount,
    });
})();

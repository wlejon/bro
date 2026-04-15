// Export final - save rigged + skinned + animated glb.

(function() {
    function mount(ps, ctx) {
        const m = ps.cleanMesh || ps.sourceMesh;
        ctx.paramsBody.innerHTML = `
            <div class="readout">mesh: ${m ? m.vertexCount + ' verts / ' + m.triangleCount + ' tris' : '-'}\nskeleton: ${ps.skeleton ? ps.skeleton.boneCount + ' bones' : 'none'}\nanim: ${ps.activeAnim ? (ps.activeAnim.duration?.toFixed?.(2) + 's') : 'none'}</div>
            <button id="save-final">Save final glTF...</button>
            <div class="readout" id="final-out">not saved</div>
        `;
        const out = document.getElementById('final-out');
        document.getElementById('save-final').addEventListener('click', () => {
            const mesh = ps.cleanMesh || ps.sourceMesh;
            if (!mesh) { ctx.setStatus('no mesh', true); return; }
            const path = showSaveFileDialog('glTF|glb', 'forge_rigged.glb');
            if (!path) return;
            try {
                const opts = {};
                if (ps.skeleton) opts.skeleton = ps.skeleton;
                if (ps.skin) opts.skin = ps.skin;
                if (ps.activeAnim) opts.animations = [ps.activeAnim];
                mesh.saveGLTF(path, opts);
                out.textContent = 'saved: ' + path;
                ctx.setStatus('final export ok');
            } catch(e) {
                ctx.setStatus('final export failed: ' + e.message, true);
            }
        });
    }
    window.Stages.push({
        id: 'export_final',
        label: '8. Export final',
        canEnter: (ps) => !!(ps.cleanMesh || ps.sourceMesh),
        mount,
    });
})();

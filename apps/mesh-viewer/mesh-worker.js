// =============================================================================
// Mesh worker — runs every heavy bromesh op off the main thread so the UI
// stays responsive. Mesh / ProgressiveMesh are available in workers (engine
// installs MeshBindings into the worker JS context).
//
// Protocol:
//   in:  { id, op, mesh, params }
//   out: { id, ok: true,  result: { mesh? | manifold? | volume? | pairs? | encoded? } }
//        { id, ok: false, error: string }
//
// `mesh` and any returned mesh are serialized as
//   { positions: Float32Array, indices: Uint32Array, normals?, uvs?, colors? }
// Returned typed-array buffers are listed in the postMessage transferList for
// zero-copy hand-off back to the main thread.
// =============================================================================

function meshFromData(d) {
    const m = new Mesh();
    m.positions = d.positions;
    m.indices   = d.indices;
    if (d.normals) m.normals = d.normals;
    if (d.uvs)     m.uvs     = d.uvs;
    if (d.colors)  m.colors  = d.colors;
    return m;
}

function serialize(m) {
    const out = {
        positions: new Float32Array(m.positions),
        indices:   new Uint32Array(m.indices),
    };
    if (m.hasNormals) out.normals = new Float32Array(m.normals);
    if (m.hasUVs)     out.uvs     = new Float32Array(m.uvs);
    if (m.hasColors)  out.colors  = new Float32Array(m.colors);
    return out;
}

function transferList(serializedMesh) {
    const list = [serializedMesh.positions.buffer, serializedMesh.indices.buffer];
    if (serializedMesh.normals) list.push(serializedMesh.normals.buffer);
    if (serializedMesh.uvs)     list.push(serializedMesh.uvs.buffer);
    if (serializedMesh.colors)  list.push(serializedMesh.colors.buffer);
    return list;
}

function reply(id, result, transfers) {
    self.postMessage({ id, ok: true, result }, transfers || []);
}

function replyMesh(id, mesh, extra) {
    const ser = serialize(mesh);
    const result = Object.assign({ mesh: ser }, extra || {});
    reply(id, result, transferList(ser));
}

self.onmessage = (e) => {
    const { id, op, mesh: meshData, params } = e.data;
    const p = params || {};
    try {
        // 'lodAt' has no input mesh — only an encoded ProgressiveMesh blob.
        if (op === 'lodAt') {
            const pm = ProgressiveMesh.deserialize(p.encoded);
            const lod = pm.atRatio(p.ratio);
            replyMesh(id, lod);
            return;
        }

        const m = meshFromData(meshData);

        switch (op) {
            case 'subdivideLoop': m.subdivideLoop(p.iters || 1); replyMesh(id, m); break;
            case 'subdivideCC':   m.subdivideCatmullClark(p.iters || 1); replyMesh(id, m); break;
            case 'subdivideMid':  m.subdivideMidpoint(p.iters || 1); replyMesh(id, m); break;

            case 'smoothLap':     m.smoothLaplacian(p.lambda || 0.5, p.iters || 5); replyMesh(id, m); break;
            case 'smoothTau':     m.smoothTaubin(p.lambda || 0.5, p.mu || -0.53, p.iters || 10); replyMesh(id, m); break;

            case 'remesh':        m.remeshIsotropic(p.edgeLen, p.iters || 3); replyMesh(id, m); break;
            case 'simplify':      m.simplify(p.ratio, p.error || 0.01); replyMesh(id, m); break;

            case 'unwrap': {
                const atlas = m.unwrapUVs();
                replyMesh(id, m, { atlas });
                break;
            }

            case 'bakeAO':    m.bakeAmbientOcclusion(p.rays || 64, p.maxDist || 0); replyMesh(id, m); break;
            case 'bakeCurv':  m.bakeCurvature(p.scale || 1.0); replyMesh(id, m); break;
            case 'bakeThick': m.bakeThickness(p.rays || 32, p.maxDist || 0); replyMesh(id, m); break;

            case 'convexHull': replyMesh(id, m.convexHull()); break;

            case 'selfInt': {
                const pairs = m.findSelfIntersections() || [];
                reply(id, { pairs });
                break;
            }

            case 'isManifold': {
                const manifold = m.isManifold();
                let volume = null;
                try { if (manifold) volume = m.computeVolume(); } catch (e) {}
                reply(id, { manifold, volume });
                break;
            }

            case 'lodBuild': {
                const pm = new ProgressiveMesh(m);
                const encoded = pm.serialize();
                reply(id, { encoded }, [encoded.buffer]);
                break;
            }

            default:
                self.postMessage({ id, ok: false, error: 'unknown op: ' + op });
        }
    } catch (err) {
        self.postMessage({ id, ok: false, error: (err && err.message) || String(err) });
    }
};

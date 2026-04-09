// Minimal test: does Mesh work in a worker?
self.onmessage = function(e) {
    var msg = e.data;
    if (msg.type === 'test') {
        try {
            var box = Mesh.box(1, 1, 1);
            self.postMessage({
                type: 'result',
                ok: true,
                vertCount: box.vertexCount,
                triCount: box.triangleCount
            });
        } catch (err) {
            self.postMessage({ type: 'result', ok: false, error: String(err) });
        }
    } else if (msg.type === 'mc') {
        try {
            // Simple density field: solid below y=4
            var G = 9;
            var field = new Float32Array(G*G*G);
            for (var z = 0; z < G; z++) {
                for (var y = 0; y < G; y++) {
                    for (var x = 0; x < G; x++) {
                        field[(z*G + y)*G + x] = (y - 4);
                    }
                }
            }
            var mesh = Mesh.marchingCubes(field, G, G, G, 0, 1.0);
            self.postMessage({
                type: 'result',
                ok: true,
                vertCount: mesh.vertexCount,
                triCount: mesh.triangleCount,
                empty: mesh.empty
            });
        } catch (err) {
            self.postMessage({ type: 'result', ok: false, error: String(err) });
        }
    }
};
self.postMessage({ type: 'ready' });

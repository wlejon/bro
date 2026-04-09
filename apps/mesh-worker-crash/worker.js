// Worker calls Mesh.box() — this is the suspected crash site
self.postMessage({ type: 'ready', stage: 'before-test' });

self.onmessage = function(e) {
    self.postMessage({ type: 'enter-onmessage' });

    if (e.data.type === 'test') {
        self.postMessage({ type: 'about-to-call-box' });
        var box = Mesh.box(1, 1, 1);
        self.postMessage({ type: 'box-returned', verts: box.vertexCount });
    }
};

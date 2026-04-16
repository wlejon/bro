self.onmessage = function(e) {
    var msg = e.data;

    if (msg.type === 'echo') {
        self.postMessage({ type: 'echo', value: msg.value });
    } else if (msg.type === 'float32') {
        self.postMessage({ type: 'float32', data: new Float32Array([1.5, 2.5, 3.5, 4.5]) });
    } else if (msg.type === 'arraybuffer') {
        var buf = new ArrayBuffer(16);
        var view = new Float32Array(buf);
        view[0] = 100; view[1] = 200; view[2] = 300; view[3] = 400;
        self.postMessage({ type: 'arraybuffer', data: buf });
    } else if (msg.type === 'mixed') {
        self.postMessage({
            type: 'mixed', name: 'test', count: 42,
            floats: new Float32Array([7.7, 8.8, 9.9]),
            ints: new Uint32Array([100, 200]),
            nested: { a: 1, b: [2, 3] }
        });
    } else if (msg.type === 'large') {
        var arr = new Float32Array(1000);
        for (var i = 0; i < 1000; i++) arr[i] = i * 0.1;
        self.postMessage({ type: 'large', data: arr, len: 1000 });
    } else if (msg.type === 'transfer') {
        var arr = new Float32Array([11.1, 22.2, 33.3]);
        self.postMessage({ type: 'transfer', data: arr }, [arr.buffer]);
    } else if (msg.type === 'echo-null') {
        self.postMessage({ type: 'echo-null', value: null });
    } else if (msg.type === 'echo-undef') {
        self.postMessage({ type: 'echo-undef', value: undefined });
    } else if (msg.type === 'nested') {
        self.postMessage({ type: 'nested', a: { b: { c: { d: 'deep' }, arr: [10, 20] } } });
    } else if (msg.type === 'self-close') {
        self.postMessage({ type: 'closing' });
        self.close();
    }
};

self.postMessage({ type: 'ready' });

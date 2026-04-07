// Simple test worker

self.onmessage = function(e) {
    var msg = e.data;

    if (msg.type === 'echo') {
        self.postMessage({ type: 'echo', value: msg.value });
    }
    else if (msg.type === 'array') {
        // Send back a plain JS array
        self.postMessage({ type: 'array', data: [1, 2, 3, 4, 5] });
    }
    else if (msg.type === 'float32') {
        // Send back a Float32Array
        var arr = new Float32Array([1.5, 2.5, 3.5, 4.5]);
        self.postMessage({ type: 'float32', data: arr });
    }
    else if (msg.type === 'uint32') {
        // Send back a Uint32Array
        var arr = new Uint32Array([10, 20, 30]);
        self.postMessage({ type: 'uint32', data: arr });
    }
    else if (msg.type === 'arraybuffer') {
        // Send back a raw ArrayBuffer
        var buf = new ArrayBuffer(16);
        var view = new Float32Array(buf);
        view[0] = 100.0; view[1] = 200.0; view[2] = 300.0; view[3] = 400.0;
        self.postMessage({ type: 'arraybuffer', data: buf });
    }
    else if (msg.type === 'mixed') {
        // Send back an object with mixed types
        var f = new Float32Array([7.7, 8.8, 9.9]);
        var u = new Uint32Array([100, 200]);
        self.postMessage({
            type: 'mixed',
            name: 'test',
            count: 42,
            floats: f,
            ints: u,
            nested: { a: 1, b: [2, 3] }
        });
    }
    else if (msg.type === 'large') {
        // Send back a larger Float32Array (like terrain data)
        var n = 1000;
        var arr = new Float32Array(n);
        for (var i = 0; i < n; i++) arr[i] = i * 0.1;
        self.postMessage({ type: 'large', data: arr, len: n });
    }
    else if (msg.type === 'transfer') {
        // Send with transferable
        var arr = new Float32Array([11.1, 22.2, 33.3]);
        self.postMessage({ type: 'transfer', data: arr }, [arr.buffer]);
    }
    else if (msg.type === 'receive-float32') {
        // Receive a Float32Array from main thread and echo info back
        var d = msg.data;
        var info = 'type=' + (d ? (d.constructor ? d.constructor.name : typeof d) : 'null');
        info += ' len=' + (d ? (d.length !== undefined ? d.length : 'undef') : 'null');
        info += ' byteLen=' + (d ? (d.byteLength !== undefined ? d.byteLength : 'undef') : 'null');
        if (d && d.length > 0) {
            info += ' v0=' + d[0];
        }
        self.postMessage({ type: 'receive-float32', info: info });
    }
    else if (msg.type === 'echo-null') {
        self.postMessage({ type: 'echo-null', value: null });
    }
    else if (msg.type === 'echo-undef') {
        self.postMessage({ type: 'echo-undef', value: undefined });
    }
    else if (msg.type === 'echo-empty-array') {
        self.postMessage({ type: 'echo-empty-array', data: [] });
    }
    else if (msg.type === 'echo-empty-obj') {
        self.postMessage({ type: 'echo-empty-obj', data: {} });
    }
    else if (msg.type === 'int8') {
        self.postMessage({ type: 'int8', data: new Int8Array([-128, 0, 127]) });
    }
    else if (msg.type === 'uint8') {
        self.postMessage({ type: 'uint8', data: new Uint8Array([0, 128, 255, 1]) });
    }
    else if (msg.type === 'float64') {
        self.postMessage({ type: 'float64', data: new Float64Array([3.141592653589793, 2.718281828459045]) });
    }
    else if (msg.type === 'int16') {
        self.postMessage({ type: 'int16', data: new Int16Array([-32768, 0, 32767]) });
    }
    else if (msg.type === 'uint16') {
        self.postMessage({ type: 'uint16', data: new Uint16Array([0, 1000, 65535]) });
    }
    else if (msg.type === 'int32') {
        self.postMessage({ type: 'int32', data: new Int32Array([-2147483648, 2147483647]) });
    }
    else if (msg.type === 'nested') {
        self.postMessage({
            type: 'nested',
            a: { b: { c: { d: 'deep' }, arr: [10, 20] } }
        });
    }
    else if (msg.type === 'self-close') {
        self.postMessage({ type: 'closing' });
        self.close();
    }
};

self.postMessage({ type: 'ready' });

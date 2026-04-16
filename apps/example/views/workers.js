window.views = window.views || {};
window.views.workers = {
    worker: null,
    init: function(el) {
        var logEl = el.querySelector('#wk-log');
        var tests = [];
        var testIdx = 0;
        var passed = 0;
        var failed = 0;
        var pending = null;

        function L(s) { logEl.textContent += s + '\n'; }
        function check(name, cond, detail) {
            if (cond) { L('  PASS: ' + name + (detail ? ' (' + detail + ')' : '')); passed++; }
            else      { L('  FAIL: ' + name + (detail ? ' (' + detail + ')' : '')); failed++; }
        }

        var worker = new Worker('views/worker.js');
        this.worker = worker;

        worker.onmessage = function(e) {
            var msg = e.data;
            if (msg.type === 'ready') { L('Worker ready, running tests...\n'); runNext(); return; }
            if (pending) { pending(msg); pending = null; runNext(); }
        };

        function send(msg, cb) { tests.push(function() { pending = cb; worker.postMessage(msg); }); }
        function runNext() {
            if (testIdx < tests.length) tests[testIdx++]();
            else L('\n--- Results: ' + passed + ' passed, ' + failed + ' failed ---');
        }

        // Echo string
        send({ type: 'echo', value: 'hello' }, function(msg) {
            L('Test: echo string');
            check('value', msg.value === 'hello', msg.value);
        });

        // Echo number
        send({ type: 'echo', value: 42 }, function(msg) {
            L('Test: echo number');
            check('value', msg.value === 42, msg.value);
        });

        // Float32Array
        send({ type: 'float32' }, function(msg) {
            L('Test: Float32Array');
            var d = msg.data;
            check('type', d && d.constructor && d.constructor.name === 'Float32Array');
            check('length', d && d.length === 4);
            check('values', d && Math.abs(d[0] - 1.5) < 0.01 && Math.abs(d[3] - 4.5) < 0.01);
        });

        // ArrayBuffer
        send({ type: 'arraybuffer' }, function(msg) {
            L('Test: ArrayBuffer');
            var d = msg.data;
            check('is ArrayBuffer', d && d.constructor && d.constructor.name === 'ArrayBuffer');
            check('byteLength', d && d.byteLength === 16);
            if (d && d.byteLength >= 16) {
                var view = new Float32Array(d);
                check('values', Math.abs(view[0] - 100) < 0.01 && Math.abs(view[3] - 400) < 0.01);
            }
        });

        // Mixed object
        send({ type: 'mixed' }, function(msg) {
            L('Test: mixed object');
            check('name', msg.name === 'test');
            check('count', msg.count === 42);
            check('floats', msg.floats && msg.floats.constructor && msg.floats.constructor.name === 'Float32Array');
            check('nested', msg.nested && msg.nested.a === 1);
        });

        // Large array
        send({ type: 'large' }, function(msg) {
            L('Test: large Float32Array (1000)');
            check('length', msg.data && msg.data.length === 1000);
            if (msg.data && msg.data.length >= 1000) {
                check('first', Math.abs(msg.data[0]) < 0.01);
                check('last', Math.abs(msg.data[999] - 99.9) < 0.1);
            }
        });

        // Transfer
        send({ type: 'transfer' }, function(msg) {
            L('Test: transferred data');
            var d = msg.data;
            var len = d ? (d.length !== undefined ? d.length : d.byteLength / 4) : 0;
            check('length', len === 3);
        });

        // Boolean
        send({ type: 'echo', value: true }, function(msg) {
            L('Test: echo boolean');
            check('value', msg.value === true);
        });

        // Null
        send({ type: 'echo-null' }, function(msg) {
            L('Test: null value');
            check('value', msg.value === null);
        });

        // Deeply nested
        send({ type: 'nested' }, function(msg) {
            L('Test: deeply nested object');
            check('depth', msg.a && msg.a.b && msg.a.b.c && msg.a.b.c.d === 'deep');
        });

        // Multiple workers
        tests.push(function() {
            L('Test: multiple workers');
            var w2 = new Worker('views/worker.js');
            w2.onmessage = function(e2) {
                if (e2.data.type === 'ready') { w2.postMessage({ type: 'echo', value: 'w2' }); return; }
                check('worker2 echo', e2.data.value === 'w2');
                w2.terminate();
                runNext();
            };
        });

        // Terminate
        tests.push(function() {
            L('Test: worker.terminate()');
            var w = new Worker('views/worker.js');
            w.onmessage = function(e3) {
                if (e3.data.type === 'ready') {
                    w.terminate();
                    check('terminated', true);
                    runNext();
                }
            };
        });
    },
    destroy: function() {
        if (this.worker) { this.worker.terminate(); this.worker = null; }
    }
};

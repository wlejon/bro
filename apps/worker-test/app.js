var log = document.querySelector('#log');
function L(s) { log.textContent += s + '\n'; console.log(s); }

var tests = [];
var testIdx = 0;
var passed = 0;
var failed = 0;

function check(name, cond, detail) {
    if (cond) {
        L('  PASS: ' + name + (detail ? ' (' + detail + ')' : ''));
        passed++;
    } else {
        L('  FAIL: ' + name + (detail ? ' (' + detail + ')' : ''));
        failed++;
    }
}

var worker = new Worker('worker.js');
var pending = null;

worker.onmessage = function(e) {
    var msg = e.data;
    if (msg.type === 'ready') {
        L('Worker ready, running tests...\n');
        runNext();
        return;
    }
    if (pending) {
        pending(msg);
        pending = null;
        runNext();
    }
};

function send(msg, cb) {
    tests.push(function() {
        pending = cb;
        worker.postMessage(msg);
    });
}

function sendWithData(msg, data, cb) {
    tests.push(function() {
        pending = cb;
        var m = {};
        for (var k in msg) m[k] = msg[k];
        m.data = data;
        worker.postMessage(m);
    });
}

function runNext() {
    if (testIdx < tests.length) {
        tests[testIdx++]();
    } else {
        L('\n--- Results: ' + passed + ' passed, ' + failed + ' failed ---');
    }
}

// ---- Define tests ----

// 1. Echo string
send({ type: 'echo', value: 'hello' }, function(msg) {
    L('Test: echo string');
    check('type', msg.type === 'echo');
    check('value', msg.value === 'hello', msg.value);
});

// 2. Echo number
send({ type: 'echo', value: 42 }, function(msg) {
    L('Test: echo number');
    check('type', msg.type === 'echo');
    check('value', msg.value === 42, msg.value);
});

// 3. Plain JS array
send({ type: 'array' }, function(msg) {
    L('Test: plain array');
    check('type', msg.type === 'array');
    check('isArray', Array.isArray(msg.data), typeof msg.data);
    check('length', msg.data && msg.data.length === 5, msg.data ? msg.data.length : 'null');
    check('values', msg.data && msg.data[0] === 1 && msg.data[4] === 5);
});

// 4. Float32Array
send({ type: 'float32' }, function(msg) {
    L('Test: Float32Array');
    check('type', msg.type === 'float32');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Float32Array', tname);
    check('length', d && d.length === 4, d ? d.length : 'null');
    check('byteLength', d && d.byteLength === 16, d ? d.byteLength : 'null');
    if (d && d.length >= 4) {
        check('values', Math.abs(d[0] - 1.5) < 0.01 && Math.abs(d[3] - 4.5) < 0.01,
              'v0=' + d[0] + ' v3=' + d[3]);
    }
});

// 5. Uint32Array
send({ type: 'uint32' }, function(msg) {
    L('Test: Uint32Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Uint32Array', tname);
    check('length', d && d.length === 3, d ? d.length : 'null');
    if (d && d.length >= 3) {
        check('values', d[0] === 10 && d[2] === 30, 'v0=' + d[0] + ' v2=' + d[2]);
    }
});

// 6. Raw ArrayBuffer
send({ type: 'arraybuffer' }, function(msg) {
    L('Test: ArrayBuffer');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('is ArrayBuffer', tname === 'ArrayBuffer', tname);
    check('byteLength', d && d.byteLength === 16, d ? d.byteLength : 'null');
    if (d && d.byteLength >= 16) {
        var view = new Float32Array(d);
        check('values', Math.abs(view[0] - 100) < 0.01 && Math.abs(view[3] - 400) < 0.01,
              'v0=' + view[0] + ' v3=' + view[3]);
    }
});

// 7. Mixed object
send({ type: 'mixed' }, function(msg) {
    L('Test: mixed object');
    check('name', msg.name === 'test', msg.name);
    check('count', msg.count === 42, msg.count);
    var ft = msg.floats ? (msg.floats.constructor ? msg.floats.constructor.name : typeof msg.floats) : 'null';
    check('floats type', ft === 'Float32Array', ft);
    check('floats len', msg.floats && msg.floats.length === 3, msg.floats ? msg.floats.length : 'null');
    var it = msg.ints ? (msg.ints.constructor ? msg.ints.constructor.name : typeof msg.ints) : 'null';
    check('ints type', it === 'Uint32Array', it);
    check('nested', msg.nested && msg.nested.a === 1, JSON.stringify(msg.nested));
});

// 8. Large Float32Array
send({ type: 'large' }, function(msg) {
    L('Test: large Float32Array (1000 elements)');
    var d = msg.data;
    check('len field', msg.len === 1000, msg.len);
    check('length', d && d.length === 1000, d ? d.length : 'null');
    check('byteLength', d && d.byteLength === 4000, d ? d.byteLength : 'null');
    if (d && d.length >= 1000) {
        check('first', Math.abs(d[0] - 0.0) < 0.01, d[0]);
        check('last', Math.abs(d[999] - 99.9) < 0.1, d[999]);
    }
});

// 9. Transfer
send({ type: 'transfer' }, function(msg) {
    L('Test: transferred Float32Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('type', tname === 'Float32Array' || tname === 'ArrayBuffer', tname);
    var len = d ? (d.length !== undefined ? d.length : (d.byteLength / 4)) : 0;
    check('length', len === 3, len);
    if (d && d.length >= 3) {
        check('values', Math.abs(d[0] - 11.1) < 0.1, d[0]);
    } else if (d && d.byteLength >= 12) {
        var v = new Float32Array(d);
        check('values (via view)', Math.abs(v[0] - 11.1) < 0.1, v[0]);
    }
});

// 10. Send Float32Array TO worker
sendWithData({ type: 'receive-float32' }, new Float32Array([99.9, 88.8, 77.7]), function(msg) {
    L('Test: send Float32Array to worker');
    check('info', msg.info && msg.info.length > 0, msg.info);
});

// 11. Boolean values
send({ type: 'echo', value: true }, function(msg) {
    L('Test: echo boolean true');
    check('value', msg.value === true, msg.value);
});
send({ type: 'echo', value: false }, function(msg) {
    L('Test: echo boolean false');
    check('value', msg.value === false, msg.value);
});

// 12. Null value
send({ type: 'echo-null' }, function(msg) {
    L('Test: null value');
    check('value', msg.value === null, msg.value);
});

// 13. Undefined value
send({ type: 'echo-undef' }, function(msg) {
    L('Test: undefined value');
    check('value', msg.value === undefined, String(msg.value));
});

// 14. Empty string
send({ type: 'echo', value: '' }, function(msg) {
    L('Test: empty string');
    check('value', msg.value === '', JSON.stringify(msg.value));
});

// 15. Empty array
send({ type: 'echo-empty-array' }, function(msg) {
    L('Test: empty array');
    check('isArray', Array.isArray(msg.data));
    check('length', msg.data && msg.data.length === 0, msg.data ? msg.data.length : 'null');
});

// 16. Empty object
send({ type: 'echo-empty-obj' }, function(msg) {
    L('Test: empty object');
    check('type', typeof msg.data === 'object' && msg.data !== null);
    check('keys', Object.keys(msg.data).length === 0, Object.keys(msg.data).length);
});

// 17. Int8Array
send({ type: 'int8' }, function(msg) {
    L('Test: Int8Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Int8Array', tname);
    check('length', d && d.length === 3, d ? d.length : 'null');
    if (d && d.length >= 3) {
        check('values', d[0] === -128 && d[1] === 0 && d[2] === 127,
              'v0=' + d[0] + ' v1=' + d[1] + ' v2=' + d[2]);
    }
});

// 18. Uint8Array
send({ type: 'uint8' }, function(msg) {
    L('Test: Uint8Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Uint8Array', tname);
    check('length', d && d.length === 4, d ? d.length : 'null');
    if (d && d.length >= 4) {
        check('values', d[0] === 0 && d[1] === 128 && d[2] === 255 && d[3] === 1);
    }
});

// 19. Float64Array
send({ type: 'float64' }, function(msg) {
    L('Test: Float64Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Float64Array', tname);
    check('length', d && d.length === 2, d ? d.length : 'null');
    if (d && d.length >= 2) {
        check('values', Math.abs(d[0] - 3.141592653589793) < 1e-10 &&
                        Math.abs(d[1] - 2.718281828459045) < 1e-10,
              'v0=' + d[0] + ' v1=' + d[1]);
    }
});

// 20. Int16Array
send({ type: 'int16' }, function(msg) {
    L('Test: Int16Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Int16Array', tname);
    check('length', d && d.length === 3, d ? d.length : 'null');
    if (d && d.length >= 3) {
        check('values', d[0] === -32768 && d[1] === 0 && d[2] === 32767);
    }
});

// 21. Uint16Array
send({ type: 'uint16' }, function(msg) {
    L('Test: Uint16Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Uint16Array', tname);
    check('length', d && d.length === 3, d ? d.length : 'null');
    if (d && d.length >= 3) {
        check('values', d[0] === 0 && d[1] === 1000 && d[2] === 65535);
    }
});

// 22. Int32Array
send({ type: 'int32' }, function(msg) {
    L('Test: Int32Array');
    var d = msg.data;
    var tname = d ? (d.constructor ? d.constructor.name : typeof d) : 'null';
    check('constructor', tname === 'Int32Array', tname);
    check('length', d && d.length === 2, d ? d.length : 'null');
    if (d && d.length >= 2) {
        check('values', d[0] === -2147483648 && d[1] === 2147483647);
    }
});

// 23. Deeply nested object
send({ type: 'nested' }, function(msg) {
    L('Test: deeply nested object');
    check('depth', msg.a && msg.a.b && msg.a.b.c && msg.a.b.c.d === 'deep',
          msg.a && msg.a.b && msg.a.b.c ? msg.a.b.c.d : 'fail');
    check('array-in-nested', msg.a && msg.a.b && msg.a.b.arr &&
          msg.a.b.arr.length === 2 && msg.a.b.arr[0] === 10,
          msg.a && msg.a.b && msg.a.b.arr ? JSON.stringify(msg.a.b.arr) : 'fail');
});

// 24. Multiple workers (concurrent)
tests.push(function() {
    L('Test: multiple workers');
    var worker2 = new Worker('worker.js');
    var gotReady = false;
    worker2.onmessage = function(e2) {
        var msg2 = e2.data;
        if (msg2.type === 'ready') {
            gotReady = true;
            worker2.postMessage({ type: 'echo', value: 'from-worker2' });
            return;
        }
        check('worker2 echo', msg2.value === 'from-worker2', msg2.value);
        worker2.terminate();
        runNext();
    };
});

// 25. worker.terminate()
tests.push(function() {
    L('Test: worker.terminate()');
    var w = new Worker('worker.js');
    var readyReceived = false;
    w.onmessage = function(e3) {
        if (e3.data.type === 'ready') {
            readyReceived = true;
            w.terminate();
            check('terminated after ready', readyReceived === true);
            // Try posting to terminated worker — should not crash
            try {
                w.postMessage({ type: 'echo', value: 'dead' });
                check('post-terminate throws or no-op', true);
            } catch(ex) {
                check('post-terminate throws', true, ex.message);
            }
            runNext();
        }
    };
});

// 26. self.close() from worker
send({ type: 'self-close' }, function(msg) {
    L('Test: self.close()');
    check('ack', msg.type === 'closing', msg.type);
});

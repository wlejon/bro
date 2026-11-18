// A Worker made from a blob: URL (URL.createObjectURL of the script's text,
// as pixi.js, three's DRACOLoader and Potree make theirs), a module worker
// ({type: 'module'}) the same way, and a data: URL worker
// (host_worker.cpp). A revoked URL is a TypeError.

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

function roundTrip(url, options, value) {
    const w = new Worker(url, options);
    let got, error;
    w.onmessage = (e) => { got = e.data; };
    w.onerror = (e) => { error = e.message || String(e); };
    w.postMessage(value);
    pumpUntil(() => got !== undefined || error !== undefined);
    w.terminate();
    return error !== undefined ? 'error: ' + error : got;
}

const classic = URL.createObjectURL(new Blob(['onmessage = function (e) { postMessage(e.data * 2); };'],
                                             { type: 'application/javascript' }));
assert(roundTrip(classic, undefined, 21) === 42, 'a classic blob worker replies');

const moduleText = 'const k = await Promise.resolve(3);\n' +
                   'self.onmessage = (e) => postMessage(e.data * k);\n' +
                   'export {};\n';
const mod = URL.createObjectURL(new Blob([moduleText], { type: 'text/javascript' }));
const r = roundTrip(mod, { type: 'module' }, 5);
assert(r === 15, 'a module blob worker replies, got ' + r);

const data = 'data:text/javascript,' + encodeURIComponent('onmessage = (e) => postMessage(e.data + 1);');
assert(roundTrip(data, undefined, 1) === 2, 'a data: URL worker replies');

URL.revokeObjectURL(classic);
let threw = '';
try { new Worker(classic); } catch (e) { threw = e.name; }
assert(threw === 'TypeError', 'a revoked object URL is a TypeError, got ' + threw);

console.log('test_blob_worker: OK');

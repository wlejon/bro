// importScripts in a classic worker (host_worker_thread.cpp): scripts by
// relative path, blob: and data: URL run in the worker's global scope in
// order before the call returns; a missing one is a NetworkError and a
// script's own throw propagates. Monaco's blob: workers start with
// importScripts(url); bro had no importScripts and they died at once.

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

const w = new Worker('../workers/worker_import_scripts.js');
let got = null, error = null;
w.onmessage = (e) => { got = e.data; };
w.onerror = (e) => { error = e.message || String(e); };
pumpUntil(() => got !== null || error !== null);
assert(error === null, 'the worker ran without an error: ' + error);
assert(got && got.type === 'function', 'importScripts is a function');
assert(got.value === 42, 'the imported functions and vars are globals, got ' + (got && got.value));
assert(got.fromBlob === 5, 'a blob: script ran too');
assert(got.order === 'start;lib;blob;', 'in order, got ' + got.order);
assert(got.missing === 'NetworkError', 'a missing script is a NetworkError, got ' + got.missing);
assert(got.thrown === 'RangeError:from import', 'a script\'s throw propagates, got ' + got.thrown);
w.terminate();

// The worker loader compiles a worker as written: a module worker whose
// async functions have lines starting with `await` keeps its imports (it was
// once wrapped in an async IIFE, which broke them), and a classic worker with
// real top-level await runs it.

function ask(path, opts, n) {
    let got = null, err = null;
    const w = new Worker(path, opts);
    w.onmessage = (e) => { got = e.data; };
    w.onerror = (e) => { err = e.message || String(e); };
    w.postMessage(n);
    const deadline = Date.now() + 15000;
    while (got === null && err === null && Date.now() < deadline) { advanceTime(1); wallSleep(2); }
    w.terminate();
    return { got, err };
}

const m = ask('../workers/worker_loader_module.js', { type: 'module' }, 21);
assert(m.err === null, 'module worker loaded without error: ' + m.err);
assert(m.got && m.got.kind === 'module' && m.got.value === 42,
       'module worker answered through its import: ' + JSON.stringify(m.got));

const t = ask('../workers/worker_loader_tla.js', undefined, 2);
assert(t.err === null, 'classic worker with top-level await loaded: ' + t.err);
assert(t.got && t.got.kind === 'tla' && t.got.value === 42,
       'top-level await ran before the first message: ' + JSON.stringify(t.got));
assert(t.got.global === 'function',
       'a classic worker\'s function declaration is a global: ' + t.got.global);

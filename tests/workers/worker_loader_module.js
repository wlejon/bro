// Companion module worker for test_worker_loader.js. It imports, and a line
// inside an async function starts with `await`: the loader must compile it as
// the module it is, not wrap it in an async function (where `import` is a
// syntax error).
import { double } from './worker_loader_dep.js';

async function reply(n) {
    await Promise.resolve();
    postMessage({ kind: 'module', value: double(n) });
}

self.onmessage = (e) => { reply(e.data); };

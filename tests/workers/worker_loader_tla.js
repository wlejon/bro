// Companion classic worker for test_worker_loader.js: real top-level await.
// Its function declarations stay globals of the worker, as a script's do.
const base = await Promise.resolve(40);

function answer(n) { return base + n; }

self.onmessage = (e) => {
    postMessage({ kind: 'tla', value: answer(e.data), global: typeof globalThis.answer });
};

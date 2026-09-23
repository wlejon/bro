// Worker for test_worker_unhandled_rejection.js: rejections nobody handles
// reach this worker's own `self` as unhandledrejection (attribute, then
// listeners), and a late handler as rejectionhandled. Every report is
// cancelled, so none is logged; what the worker saw goes back to the page.

const log = [];
let late = null;

self.onunhandledrejection = (e) => {
    log.push('attr:' + e.type + ':' + String(e.reason && e.reason.message || e.reason));
};
self.addEventListener('unhandledrejection', (e) => {
    log.push('listener:' + e.type + ':' + (e.promise === late ? 'late' : 'other') +
             ':' + e.cancelable);
    e.preventDefault();
});
self.addEventListener('rejectionhandled', (e) => {
    log.push('handled:' + (e.promise === late ? 'late' : 'other'));
});

self.onmessage = (e) => {
    const d = e.data;
    if (d.cmd === 'reject') {
        late = Promise.reject(new Error('in-worker'));
        const caughtInTime = Promise.reject(new Error('caught'));
        caughtInTime.catch(() => {});
    } else if (d.cmd === 'handle') {
        late.catch(() => {});
    } else if (d.cmd === 'log') {
        self.postMessage({ log });
    }
};

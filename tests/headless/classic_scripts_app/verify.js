// Driver for test_classic_scripts.js: waits for the page's workers, adds
// what two control evals see, and prints the page's results.
function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}
function run(cmd, arg) {
    const id = controlCommand(cmd, arg);
    let r = null;
    pumpUntil(() => (r = controlResult(id)) !== null && r !== undefined, 5000);
    return r;
}
pumpUntil(() => R.worker !== undefined && R.moduleWorker !== undefined);
// A bro-ctl eval is a unit of its own, not a classic script: what it declares
// with var or function is not the global object's while it runs, and is once
// it has run.
run('eval', 'var ctlVar = 3; function ctlFn() { return 4; } R.ctlDuring = typeof window.ctlVar;');
R.ctl = R.ctlDuring + ',' + run('eval', '[typeof ctlVar, typeof window.ctlFn].join()').payload;
console.log('CLASSIC_OUT ' + JSON.stringify(R));

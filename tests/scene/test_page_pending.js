// scene.setPendingWork(kind, count): what the page still produces for a
// scene is in its pendingWork() as "kind: count" while count is over 0, so
// perf.settle waits for it, and gone at 0.

const cv = document.createElement('canvas');
cv.setAttribute('width', '32');
cv.setAttribute('height', '32');
document.body.appendChild(cv);
flush();
const scene = cv.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({ fov: 50, near: 0.1, far: 100, position: [0, 2, 3], target: [0, 0, 0] });
    scene.captureFrame();
    const has = (line) => scene.pendingWork().includes(line);
    scene.setPendingWork('cloud light', 3);
    assert(has('cloud light: 3'), `the page's work is pending (${JSON.stringify(scene.pendingWork())})`);
    scene.setPendingWork('cloud light', 1);
    assert(has('cloud light: 1') && !has('cloud light: 3'), 'a new count replaces the old');
    // perf.settle waits on it: held at 1, it stalls out at its frame limit.
    const held = perf.settle({ maxFrames: 20, stallFrames: 10 });
    assert(!held.settled && held.pending.includes('cloud light: 1'),
           `settle waits for the page's work (${JSON.stringify(held)})`);
    scene.setPendingWork('cloud light', 0);
    assert(!scene.pendingWork().some((l) => l.startsWith('cloud light')), 'at 0 it is gone');
    let threw = null;
    try { scene.setPendingWork('', 1); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'an empty kind is a TypeError');
}

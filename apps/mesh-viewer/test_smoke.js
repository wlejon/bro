// Headless smoke test — verifies app.js parses and initial state is sane.
// Run: bro-headless apps/mesh-viewer apps/mesh-viewer/test_smoke.js

assert(typeof Mesh === 'function' || typeof Mesh === 'object', 'Mesh global');
assert(typeof ProgressiveMesh === 'function', 'ProgressiveMesh global');
assert(typeof Pose === 'function' || typeof Pose === 'object', 'Pose global');
assert(typeof Worker === 'function', 'Worker global');

// App globals
assert(typeof state === 'object',         'state exists');
assert(state.loaded === null,             'no file loaded yet');
assert(state.busy === false,              'not busy');
assert(typeof runModify === 'function',   'runModify wired');
assert(typeof applyColorMode === 'function', 'applyColorMode wired');
assert(typeof exportMesh === 'function',  'exportMesh wired');
assert(typeof drawUVInset === 'function', 'drawUVInset wired');
assert(typeof setHullVisible === 'function', 'setHullVisible wired');
assert(typeof setSelfxVisible === 'function', 'setSelfxVisible wired');
assert(typeof buildLODChain === 'function', 'buildLODChain wired');
assert(typeof runStatsChecks === 'function', 'runStatsChecks wired');
assert(typeof renderStats === 'function', 'renderStats wired');
assert(typeof setupMenu === 'function', 'setupMenu wired');

// Worker created.
assert(typeof worker === 'object',     'worker created');
assert(typeof worker.postMessage === 'function', 'worker has postMessage');

// Stats panel renders without a loaded file.
renderStats();
assert(document.getElementById('st-meshes').textContent === '0', 'meshes shows 0');
assert(document.getElementById('st-tris').textContent === '0',   'tris shows 0');

// View mode select wired.
const sel = document.getElementById('view-mode');
assert(sel && sel.value === 'original', 'view mode select default');

// Stats helpers handle missing data without throwing.
applyColorMode('original');           // returns immediately when nothing loaded
setHullVisible(false);
setSelfxVisible(false);
setUVVisible(false);

// Menu was set up (file menu exists).
assert(typeof bro === 'object', 'bro global');
assert(typeof bro.menu === 'object', 'bro.menu');

console.log('smoke OK');

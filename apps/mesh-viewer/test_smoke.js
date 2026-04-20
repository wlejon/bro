// Headless smoke test — verifies app.js parses and initial state is sane.
// Run: bro-headless apps/mesh-viewer apps/mesh-viewer/test_smoke.js

assert(typeof Mesh === 'function' || typeof Mesh === 'object', 'Mesh global');
assert(typeof ProgressiveMesh === 'function', 'ProgressiveMesh global');
assert(typeof Pose === 'function' || typeof Pose === 'object', 'Pose global');

// App globals
assert(typeof state === 'object',         'state exists');
assert(state.loaded === null,             'no file loaded yet');
assert(typeof applyMeshOp === 'function', 'applyMeshOp wired');
assert(typeof applyColorMode === 'function', 'applyColorMode wired');
assert(typeof exportMesh === 'function',  'exportMesh wired');
assert(typeof drawUVInset === 'function', 'drawUVInset wired');
assert(typeof setHullVisible === 'function', 'setHullVisible wired');
assert(typeof setSelfxVisible === 'function', 'setSelfxVisible wired');
assert(typeof buildLODChain === 'function', 'buildLODChain wired');
assert(typeof renderStats === 'function', 'renderStats wired');

// Stats panel renders without a loaded file.
renderStats();
const fileVal = document.getElementById('st-file').textContent;
assert(fileVal === '\u2014', 'stats file shows em-dash when empty (got: ' + fileVal + ')');

// View mode select wired.
const sel = document.getElementById('view-mode');
assert(sel && sel.value === 'original', 'view mode select default');

// Stats helpers handle missing data without throwing.
applyColorMode('original');           // no-op when nothing loaded
setHullVisible(false);
setSelfxVisible(false);

console.log('smoke OK');

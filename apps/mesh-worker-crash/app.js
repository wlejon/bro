// Minimal repro: spawn one worker, ask it to call Mesh.box()
var log = document.getElementById('log');
function L(s) { console.log(s); if (log) log.textContent += s + '\n'; }

L('main: spawning worker...');
var w = new Worker('worker.js');

w.onmessage = function(e) {
    L('main: got ' + JSON.stringify(e.data));
};

setTimeout(function() {
    L('main: posting test message');
    w.postMessage({ type: 'test' });
}, 200);

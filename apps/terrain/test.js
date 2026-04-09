// Smoke test: spawn a worker that uses Mesh, verify it responds
console.log('=== terrain worker test ===');

var w = new Worker('test-mesh-worker.js');
var got = [];

w.onmessage = function(e) {
    console.log('main got:', JSON.stringify(e.data));
    got.push(e.data);
};

// Wait for ready
advanceTime(1000);
flush();

console.log('After 1s, got count:', got.length);

// Send test message
console.log('Sending test message...');
w.postMessage({ type: 'test' });
advanceTime(2000);
flush();

console.log('After test, got count:', got.length);

// Send marching cubes test
console.log('Sending mc message...');
w.postMessage({ type: 'mc' });
advanceTime(2000);
flush();

console.log('After mc, got count:', got.length);
console.log('=== done ===');

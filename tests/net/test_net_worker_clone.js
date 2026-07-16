// Worker <-> host structured-value messaging over the network.
//
// The main context hosts; a Worker (its own JS context + net subscriber on
// its own thread) connects over loopback and sendClones a structured value on
// channel 2. The host verifies the decoded value + channel, sendClones a
// reply on channel 4, and the worker reports the decoded reply back over
// postMessage. This proves per-context correctness: each side serializes in
// its own JSContext and deserializes in the receiving JSContext.

const PORT = 27900 + (Date.now() % 400);

function pump(realMs, virtualMs) {
    wallSleep(realMs);
    advanceTime(virtualMs);
}

let fromWorker = null;
let workerResult = null;

bro.net.onmessage = (conn, data, channel) => {
    fromWorker = { conn, data, channel };
};

assert(bro.net.host(PORT) !== false, 'host() accepted');
for (let i = 0; i < 200 && !bro.net.isHosting(); i++) pump(10, 10);
assert(bro.net.isHosting(), 'hosting on port ' + PORT);

const w = new Worker('../net/worker_net_clone.js');
w.onmessage = (e) => { workerResult = e.data; };
w.postMessage({ port: PORT });

// Wait for the worker's clone to land.
for (let i = 0; i < 800 && fromWorker === null; i++) pump(10, 10);
assert(fromWorker !== null, 'host received the worker clone');
assert(fromWorker.channel === 2, 'worker clone arrived on channel 2 (got ' + fromWorker.channel + ')');
const v = fromWorker.data;
assert(!(v instanceof ArrayBuffer), 'worker clone decoded to a value');
assert(v.greeting === 'from-worker', 'string field roundtrips');
assert(v.pos instanceof Float32Array && v.pos.length === 3 && v.pos[1] === -2.5,
       'Float32Array roundtrips from worker context');
assert(v.meta.seq === 1 && v.meta.tags[1] === 'b', 'nested object roundtrips');

// Reply with a clone of our own — deserialized in the WORKER's context.
bro.net.sendClone(fromWorker.conn, {
    ack: true,
    echo: { seq: v.meta.seq },
    sum: v.pos[0] + v.pos[1] + v.pos[2],
}, { channel: 4 });

for (let i = 0; i < 800 && workerResult === null; i++) pump(10, 10);
assert(workerResult !== null, 'worker reported back');
assert(workerResult.done === true, 'worker completed: ' + JSON.stringify(workerResult));
assert(workerResult.isValue === true, 'worker received a decoded value, not bytes');
assert(workerResult.ack === true, 'ack field roundtrips to worker');
assert(workerResult.echoSeq === 1, 'nested echo roundtrips to worker');
assert(workerResult.sum === 2.5, 'computed float roundtrips (got ' + workerResult.sum + ')');
assert(workerResult.channel === 4, 'reply arrived on channel 4 (got ' + workerResult.channel + ')');

w.terminate();
bro.net.close();
advanceTime(100);
console.log('[test] PASS');

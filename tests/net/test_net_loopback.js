// Loopback flood: the host must receive every message a peer sends, even when
// the peer sends far more of them than the service->JS event ring can hold.
//
// The ring is 1024 slots. This sends 3000 reliable messages while the host is
// deliberately not polling, so the backlog cannot fit. The service thread used
// to pull messages out of GNS regardless and delete the ones that didn't fit —
// once out of GNS they are gone, so those were lost for good while the sender
// still counted them as delivered. It now only pulls what the ring can hold and
// leaves the rest in GNS, which is what makes this test pass.
//
// Each payload is its own index, so "nothing was lost" is checked exactly
// rather than by count alone.

const PORT = 27050 + (Date.now() % 400);   // avoid colliding with a stale socket
const COUNT = 3000;                         // ~3x the 1024-slot ring

const received = [];
bro.net.onmessage = (conn, data) => {
    received.push(new DataView(data).getUint32(0, true));
};

assert(bro.net.host(PORT) !== false, 'host() accepted');

// Pump until the listen socket is actually up.
let waited = 0;
while (!bro.net.isHosting() && waited < 3000) {
    advanceTime(16);
    wallSleep(2);
    waited += 16;
}
assert(bro.net.isHosting() === true, 'hosting on port ' + PORT);

// Start the flood. The worker runs its own event loop on its own thread, so it
// connects and sends without any help from this thread.
const w = new Worker('../net/worker_net_flood.js');
let workerDone = null;
w.onmessage = (e) => { workerDone = e.data; };
w.postMessage({ port: PORT, count: COUNT });

// Deliberately stall. advanceTime() is what runs the frame pump that drains the
// event ring, so wallSleep() alone lets the flood pile up with nobody draining
// it — exactly the condition under which messages used to be destroyed.
wallSleep(1000);
assert(received.length === 0, 'host did not drain while stalled');

// Now drain.
waited = 0;
while (received.length < COUNT && waited < 20000) {
    advanceTime(16);
    wallSleep(2);
    waited += 16;
}

assert(workerDone !== null, 'worker reported back');
assert(workerDone.done === true, 'worker connected and sent: ' + JSON.stringify(workerDone));
assert(workerDone.sent === COUNT, 'worker sent all ' + COUNT);

// The whole point: nothing was silently dropped.
assert(received.length === COUNT,
       'received every message — got ' + received.length + ' of ' + COUNT);

// Reliable delivery is ordered, and each index must appear exactly once.
let ordered = true;
for (let i = 0; i < COUNT; i++) {
    if (received[i] !== i) { ordered = false; break; }
}
assert(ordered === true, 'messages arrived in order, none duplicated or missing');

w.terminate();
bro.net.close();

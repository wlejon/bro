// bro.remote hosting from bro-headless, driven by bro_remote_host_test
// (src/bronze_host/tests/test_remote_host.cpp), which runs this script on
// tests/remote/app and is the viewer: a broremote Client on the socket named
// by BRO_REMOTE_TEST_SOCKET. It checks the frames it receives against the
// page and sends a key and a click; this side checks that they arrived as DOM
// events and that bro.remote reported the session. It exits when the viewer
// has detached (or after a minute).
const socket = process.env.BRO_REMOTE_TEST_SOCKET;
assert(typeof socket === 'string' && socket.length > 0, 'BRO_REMOTE_TEST_SOCKET names the socket');
assert(bro.remote.available === true, 'bro.remote is available');

const events = [];
bro.remote.on('attach', (e) => events.push(e.type + ':' + e.clients));
bro.remote.ondetach = (e) => events.push(e.type + ':' + e.clients);

const started = bro.remote.host({ socket, codecs: 'raw', fps: 30 });
assert(started.hosting === true && started.socket === socket, 'host() reports hosting on the socket');

let stream = null;
const t0 = Date.now();
while (!events.includes('detach:0') && Date.now() - t0 < 60000) {
    advanceTime(16);
    wallSleep(4);
    if (!stream) {
        const s = bro.remote.status();
        if (s.codec) stream = s;
    }
}

const seen = window.remoteSeen;
assert(events.join() === 'attach:1,detach:0', 'attach, then detach: ' + events.join());
assert(stream && stream.codec === 'raw' && stream.width === 320 && stream.height === 240,
       'status() reports the stream: ' + JSON.stringify(stream));
assert(stream && stream.stats.encoded >= 1, 'frames were encoded');
assert(seen.keys.join() === 'a', 'the viewer\'s key arrived as keydown "a": ' + JSON.stringify(seen.keys));
assert(seen.clicks.length === 1 && seen.clicks[0][0] === 100 && seen.clicks[0][1] === 60 &&
       seen.clicks[0][2] === 0, 'the viewer\'s click arrived at (100, 60): ' + JSON.stringify(seen.clicks));
assert(bro.remote.stop() === true, 'stop() stops the server');
assert(bro.remote.status().hosting === false, 'status() after stop()');

// The viewer side of bro.remote, from bro-headless: a <remoteview> showing a
// bro.remote.connect() session to the bro-headless that hosts tests/remote/app
// (host_remote.js), both started by bro_remote_host_test
// (src/bronze_host/tests/test_remote_host.cpp). The host's page is one colour
// per state (idle, keyed, clicked); this side sees them through the view,
// sends the key and the click the host's script checks for, and checks that
// the key never reached its own page (a captured view takes keys raw), the
// release chord, and the view's stats. The host exits once this one closes.
const socket = process.env.BRO_REMOTE_TEST_SOCKET;
assert(typeof socket === 'string' && socket.length > 0, 'BRO_REMOTE_TEST_SOCKET names the socket');
assert(typeof bro.remote.connect === 'function', 'bro.remote.connect exists');
assert(typeof HTMLRemoteViewElement === 'function', 'HTMLRemoteViewElement exists');

const view = document.createElement('remoteview');
assert(view instanceof HTMLRemoteViewElement, 'a <remoteview> is an HTMLRemoteViewElement');
view.style.cssText = 'position: fixed; left: 0; top: 0; width: 320px; height: 240px; display: block;';
document.body.appendChild(view);
// This page is the host's own app: black here, so only the view shows its colours.
document.body.style.background = 'rgb(0, 0, 0)';
const seenEvents = [];
view.addEventListener('capture', () => seenEvents.push('capture'));
view.addEventListener('release', () => seenEvents.push('release'));

function pump(ms, pred) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        advanceTime(16);
        wallSleep(4);
        if (pred()) return true;
    }
    return false;
}

// The host compiles its page first: connect until it answers.
let session = null;
const t0 = Date.now();
while (Date.now() - t0 < 90000) {
    session = bro.remote.connect({ socket, audio: false, name: 'view_remote.js' });
    view.session = session;
    pump(10000, () => session.status().state !== 'connecting');
    if (session.status().state === 'connected') break;
    session.close();
    session = null;
    wallSleep(200);
}
assert(session !== null, 'connected to the host');
assert(view.session === session, 'view.session is the session');

const idle = [32, 80, 160], keyed = [160, 32, 80], clicked = [32, 160, 80];
const near = (p, c) => Math.abs(p.r - c[0]) <= 3 && Math.abs(p.g - c[1]) <= 3 && Math.abs(p.b - c[2]) <= 3;
const shows = (c) => near(getPixel(160, 120), c) && near(getPixel(20, 20), c) && near(getPixel(300, 220), c);

assert(pump(30000, () => view.streamWidth > 0 && shows(idle)),
       'the view shows the host\'s page: ' + JSON.stringify(getPixel(160, 120)) + ' ' + JSON.stringify(view.stats()));
assert(view.streamWidth === 320 && view.streamHeight === 240,
       'the stream size: ' + view.streamWidth + 'x' + view.streamHeight);
const st = session.status();
assert(st.state === 'connected' && st.codec === 'raw' && st.width === 320, 'status(): ' + JSON.stringify(st));

// A captured view takes keys before the page: the host gets them, this page not.
view.capture();
flush();
assert(view.captured === true, 'capture() captures');
assert(document.activeElement === view, 'capture() focuses the view');
keyDown(97 /* a */, 4 /* A */, 0);
keyUp(97, 4, 0);
assert(pump(20000, () => shows(keyed)), 'the host saw the key: ' + JSON.stringify(getPixel(160, 120)));
assert(window.remoteSeen.keys.length === 0, 'the key did not reach this page: ' + JSON.stringify(window.remoteSeen.keys));

// The release chord gives the keyboard back.
keyDown(27 /* Escape */, 41 /* Escape */, 0x0040 | 0x0100 /* LCtrl | LAlt */);
keyUp(27, 41, 0x0040 | 0x0100);
pump(200, () => !view.captured);
assert(view.captured === false, 'Ctrl+Alt+Escape releases the view');
assert(seenEvents.join() === 'capture,release', 'capture and release events: ' + seenEvents.join());

// The pointer goes through the letterbox (here 1:1) to stream pixels.
click(100, 60);
assert(pump(20000, () => shows(clicked)), 'the host saw the click: ' + JSON.stringify(getPixel(160, 120)));
assert(view.captured === true, 'a press captures the view again');

const stats = view.stats();
assert(stats.pictures >= 3 && stats.presented >= 0, 'stats(): ' + JSON.stringify(stats));
assert(stats.path === 'cpu' || stats.path === 'gpu', 'stats().path: ' + stats.path);
const vs = session.stats();
assert(vs.decoded >= 3, 'session stats(): ' + JSON.stringify(vs));

session.close();
assert(view.session === null, 'a closed session leaves the view');

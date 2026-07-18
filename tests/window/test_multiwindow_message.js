// bro.window — messaging between the app realm and a secondary window's realm,
// self-close from the child, parent-side 'resize', and the child app's own
// bro.json supplying window defaults (multiwindow v1, chunk 4).
//
// Delivery is asynchronous by design: both directions queue and drain at the
// engine's idle point, which flush() runs. The drain does children first and
// the parent second, so a reply posted from a child's 'message' handler lands
// in the SAME flush() — one full round trip per flush.

// ---- open the echo child ---------------------------------------------------
const win = bro.window.open('multiwin_msg', { width: 120, height: 60 });
let loaded = 0;
win.addEventListener('load', () => { loaded++; });
flush();
assert(loaded === 1, 'child loaded');

const inbox = [];
const onMessage = (ev) => {
    assert(ev.type === 'message', "handle event type is 'message'");
    assert(ev.target === win, 'message event target is the handle');
    inbox.push(ev.data);
};
win.addEventListener('message', onMessage);

// ---- parent → child → parent round trip ------------------------------------
win.postMessage({ hello: 'world', n: 7 });
assert(inbox.length === 0, 'delivery is asynchronous — nothing before the drain');
flush();
assert(inbox.length === 1, 'one reply after the drain, got ' + inbox.length);
const first = inbox[0];
assert(first.kind === 'echo', 'child echoed the message');
assert(first.data.hello === 'world' && first.data.n === 7,
       'structured clone survived both hops: ' + JSON.stringify(first.data));
assert(first.seq === 1, 'child saw exactly one message so far');
assert(first.hasParent === true, 'child realm has bro.window.parent.postMessage');
assert(first.canOpen === false,
       'bro.window.open still throws in a secondary window realm');

// The clone really is a copy — mutating the reply cannot touch the child.
assert(first.data !== undefined && typeof first.data === 'object',
       'cloned payload is a fresh object');

// ---- ordering: several messages arrive in post order -----------------------
inbox.length = 0;
win.postMessage({ i: 1 });
win.postMessage({ i: 2 });
win.postMessage({ i: 3 });
flush();
assert(inbox.length === 3, 'all three replies arrived, got ' + inbox.length);
assert(inbox[0].data.i === 1 && inbox[1].data.i === 2 && inbox[2].data.i === 3,
       'replies preserve post order: ' + inbox.map((m) => m.data.i).join(','));
assert(inbox[0].seq === 2 && inbox[2].seq === 4,
       'child handled them in order too: ' + inbox.map((m) => m.seq).join(','));

// ---- ArrayBuffer transfer: zero-copy out, detached on this side ------------
inbox.length = 0;
const buf = new ArrayBuffer(4);
new Uint8Array(buf).set([10, 20, 30, 40]);
win.postMessage({ buf: buf }, [buf]);
assert(buf.byteLength === 0, 'transferred ArrayBuffer is detached on the sender');
flush();
assert(inbox.length === 1, 'transfer produced one reply');
assert(inbox[0].kind === 'sum' && inbox[0].len === 4 && inbox[0].sum === 100,
       'child received the transferred bytes: ' + JSON.stringify(inbox[0]));

// A non-cloneable value is a clean TypeError, not a silent drop.
let threw = false;
try { win.postMessage({ fn: function () {} }); } catch (e) { threw = true; }
assert(threw, 'posting a function throws (not cloneable)');

// ---- removeEventListener stops delivery ------------------------------------
inbox.length = 0;
win.removeEventListener('message', onMessage);
win.postMessage({ i: 99 });
flush();
assert(inbox.length === 0, 'removed listener no longer receives messages');
win.addEventListener('message', onMessage);

// ---- parent observes 'resize' ---------------------------------------------
const resizes = [];
win.addEventListener('resize', (ev) => {
    assert(ev.target === win, 'resize event target is the handle');
    resizes.push([ev.width, ev.height]);
});
win.setSize(200, 90);
flush();
assert(resizes.length === 1, "one 'resize' on the parent handle, got " + resizes.length);
assert(resizes[0][0] === 200 && resizes[0][1] === 90,
       'resize event carries the new size: ' + resizes[0].join('x'));
flush();
assert(resizes.length === 1, 'resize does not re-fire when nothing changed');

// ---- posting to a closed window is a no-op, not a crash --------------------
win.close();
flush();
assert(win.closed === true, 'window closed');
inbox.length = 0;
win.postMessage({ after: 'close' });   // must not throw
flush();
assert(inbox.length === 0, 'no delivery to (or from) a closed window');
// Transfers still detach even when the destination is gone — the clone is what
// detaches, and that must not depend on the window's liveness.
const orphan = new ArrayBuffer(2);
win.postMessage({ b: orphan }, [orphan]);
assert(orphan.byteLength === 0, 'transfer to a closed window still detaches');
flush();

// ---- window.close() from the child realm -----------------------------------
const selfCloser = bro.window.open('multiwin_msg', { width: 80, height: 40 });
let selfClosed = 0;
selfCloser.addEventListener('close', (ev) => {
    selfClosed++;
    assert(ev.target === selfCloser, 'close event target is the handle');
    assert(selfCloser.closed === true, 'closed reads true inside the listener');
});
flush();
assert(selfCloser.closed === false, 'self-closer is open');
selfCloser.postMessage('close-me');
flush();   // delivers the message; the child queues its own close
flush();   // the queued close runs at this drain
assert(selfCloser.closed === true, 'child window.close() closed its own window');
assert(selfClosed === 1,
       "parent handle got exactly one 'close', got " + selfClosed);

// ---- the child app's bro.json supplies window defaults ---------------------
const fromManifest = bro.window.open('multiwin_manifest');
const mInbox = [];
fromManifest.addEventListener('message', (ev) => { mInbox.push(ev.data); });
flush();
let size = fromManifest.getSize();
assert(size.width === 321 && size.height === 123,
       "child bro.json width/height honored: " + size.width + 'x' + size.height);
fromManifest.postMessage('size?');
flush();
assert(mInbox.length === 1 && mInbox[0].innerWidth === 321 &&
       mInbox[0].innerHeight === 123,
       'the child realm was laid out at its manifest size: ' +
       JSON.stringify(mInbox[0]));
fromManifest.close();
flush();

// ---- explicit options win over the child's bro.json ------------------------
// (Sizes stay above the manifest's minWidth/minHeight, which the window also
// inherits — asking for less would be clamped by the OS window, not by us.)
const override = bro.window.open('multiwin_manifest', { width: 260, height: 160 });
flush();
size = override.getSize();
assert(size.width === 260 && size.height === 160,
       'explicit open() options override the child manifest: ' +
       size.width + 'x' + size.height);
override.close();
flush();

console.log('multiwindow messaging OK');

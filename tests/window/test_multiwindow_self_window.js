// A secondary window's realm drives its OWN window through bro.window: it
// reads back the flags and limits its bro.json declared, its setters land on
// it and leave the main window alone.

const hostBefore = {
    borderless: bro.window.borderless,
    alwaysOnTop: bro.window.alwaysOnTop,
    min: bro.window.getMinSize(),
    max: bro.window.getMaxSize(),
};

const win = bro.window.open('multiwin_self');
let reply = null;
win.addEventListener('message', (ev) => { reply = ev.data; });
flush();
assert(!win.closed, 'child opened');

function ask(op) {
    reply = null;
    win.postMessage(op);
    flush();
    assert(reply !== null, 'child replied to ' + JSON.stringify(op));
    return reply;
}

// The child's bro.json reached its window, and its realm reads it back.
let r = ask({ op: 'report' });
assert(r.borderless === true && r.alwaysOnTop === true,
       'child reads its manifest flags: ' + JSON.stringify(r));
assert(r.min.width === 210 && r.min.height === 110, 'child min size ' + JSON.stringify(r.min));
assert(r.max.width === 1500 && r.max.height === 950, 'child max size ' + JSON.stringify(r.max));
assert(r.size.width === 300 && r.size.height === 200, 'child size ' + JSON.stringify(r.size));

// Its setters land on it ...
r = ask({ op: 'minSize', w: 280, h: 240 });
assert(r.min.width === 280 && r.min.height === 240, 'child min size set ' + JSON.stringify(r.min));
r = ask({ op: 'borderless', v: false });
assert(r.borderless === false, 'child borderless cleared');
r = ask({ op: 'alwaysOnTop', v: false });
assert(r.alwaysOnTop === false, 'child alwaysOnTop cleared');
r = ask({ op: 'size', w: 360, h: 260 });
assert(r.size.width === 360 && r.size.height === 260, 'child size set ' + JSON.stringify(r.size));
const hs = win.getSize();
assert(hs.width === 360 && hs.height === 260, 'the parent handle sees the new size ' + JSON.stringify(hs));

// ... and not on the main window.
assert(bro.window.borderless === hostBefore.borderless, 'host borderless untouched');
assert(bro.window.alwaysOnTop === hostBefore.alwaysOnTop, 'host alwaysOnTop untouched');
const hm = bro.window.getMinSize();
assert(hm.width === hostBefore.min.width && hm.height === hostBefore.min.height,
       'host min size untouched: ' + JSON.stringify(hm) + ' vs ' + JSON.stringify(hostBefore.min));
const hx = bro.window.getMaxSize();
assert(hx.width === hostBefore.max.width && hx.height === hostBefore.max.height, 'host max size untouched');

win.postMessage({ op: 'close' });
flush();
flush();
assert(win.closed, 'child closed itself');

console.log('test_multiwindow_self_window: OK');

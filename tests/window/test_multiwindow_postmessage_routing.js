// window.postMessage across bro windows: every message lands at the window it
// was posted TO, never at the main window by default.
//
//   - a secondary window's own window.postMessage reaches that window (source
//     = itself), not the main document;
//   - the opener's handle.postMessage reaches the opened window as a real
//     MessageEvent, source = its window.opener, origin = the page's;
//   - the opened window's window.opener.postMessage reaches the main window,
//     source = the handle bro.window.open returned;
//   - the main window's own self-post stays in the main window.
//
// Child: tests/test_app/multiwin_route. Delivery is a task, so each step
// advances a frame and flushes (the window-message drain runs in both).

function pump() { advanceTime(20); flush(); advanceTime(20); flush(); }

assert(window.opener === null, 'the main window has no opener');

const mainInbox = [];
window.addEventListener('message', (e) => {
    mainInbox.push({ data: e.data, source: e.source, origin: e.origin,
                     isMessageEvent: e instanceof MessageEvent });
});

const win = bro.window.open('multiwin_route', { width: 120, height: 60 });
pump();
assert(mainInbox.length === 0,
       "the child's self-post never reaches the main window, got " +
       JSON.stringify(mainInbox.map((m) => m.data)));

// ---- opener → child, then child → opener -------------------------------------
win.postMessage({ from: 'opener' }, '*');
win.postMessage('report', '*');
pump();
assert(mainInbox.length === 1, 'one report at the main window, got ' + mainInbox.length);
const rep = mainInbox[0];
assert(rep.isMessageEvent, 'the report is a MessageEvent');
assert(rep.source === win, "the report's source is the handle bro.window.open returned");
assert(rep.origin === 'bro://app', 'report origin, got ' + rep.origin);
assert(rep.data.openerIsObject && rep.data.openerStable, 'window.opener is one stable object');

const log = rep.data.log;
assert(log.length === 2, 'the child saw its self-post and the opener message, got ' +
       JSON.stringify(log));
const [self, fromOpener] = log;
assert(self.data.self === 'child', 'first: its own self-post');
assert(self.sourceIsSelf && !self.sourceIsOpener, "a self-post's source is the child's window");
assert(self.isMessageEvent && self.docIsMine,
       'self-post delivered as a MessageEvent in the child realm: ' + JSON.stringify(self));
assert(self.origin === 'bro://app', 'self-post origin');
assert(fromOpener.data.from === 'opener', 'second: the opener message');
assert(fromOpener.isMessageEvent, 'the opener message is a MessageEvent');
assert(fromOpener.sourceIsOpener && !fromOpener.sourceIsSelf,
       "the opener message's source is window.opener");
assert(fromOpener.origin === 'bro://app', 'opener message origin, got ' + fromOpener.origin);
assert(fromOpener.docIsMine, 'the opener message runs with the child document current');
// onmessage runs before the listeners, so it has also seen 'report' itself.
assert(rep.data.viaOn === 3, 'the child onmessage saw all three messages, got ' + rep.data.viaOn);

// ---- the main window's self-post stays in the main window ----------------------
mainInbox.length = 0;
window.postMessage('main-self', '*');
pump();
assert(mainInbox.length === 1 && mainInbox[0].data === 'main-self' &&
       mainInbox[0].source === window, 'main self-post reaches the main window');
mainInbox.length = 0;
win.postMessage('report', '*');
pump();
assert(mainInbox.length === 1, 'second report arrived');
assert(mainInbox[0].data.log.length === 2,
       'the child never saw the main window self-post: ' +
       JSON.stringify(mainInbox[0].data.log.map((m) => m.data)));

// ---- targetOrigin applies between windows too -----------------------------------
mainInbox.length = 0;
win.postMessage({ from: 'elsewhere' }, 'https://example.com');
win.postMessage('report', '/');
pump();
assert(mainInbox.length === 1 && mainInbox[0].data.log.length === 2,
       'a message for another origin is dropped');
let threw = null;
try { win.postMessage('x', 'not a url'); } catch (e) { threw = e; }
assert(threw && threw.name === 'SyntaxError', 'a bad targetOrigin is a SyntaxError');

win.close();
pump();
assert(win.closed, 'child closed');
console.log('multiwindow postMessage routing OK');

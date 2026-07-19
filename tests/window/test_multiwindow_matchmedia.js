// bro.window — matchMedia inside a secondary window's realm.
//
// Regression test for a two-part gap that made a listener in a secondary
// window silently never fire while `.matches` read correctly the whole time:
//
//   1. deliverMediaQueryChangesAllRealms() / applyColorScheme() walked the app
//      document, iframe sub-documents and system panels, but not windowHosts_.
//      A host realm therefore never had its scheme updated or its lists
//      re-evaluated.
//   2. Even once delivery reached the right JSContext, change events wait on
//      Document::mediaRestylePending(), which only clears in resolveStyles().
//      For a host document that runs from recordWindowHostLayers(), called
//      solely from the windowed frame loop — so under headless the flag was
//      set by the scheme flip and never cleared, and delivery bailed forever.
//
// The child reports both a live `.matches` read and its delivered change
// events, so a regression in either half fails here rather than going quiet.

const child = 'mql_window_child';

function ask(win, inbox) {
    inbox.length = 0;
    win.postMessage({ ask: 'state' });
    flush();
    assert(inbox.length === 1,
           'child answered the state probe, got ' + inbox.length + ' replies');
    return inbox[0];
}

bro.settings.set('appearance.colorScheme', 'light');
flush();

const inbox = [];
const win = bro.window.open(child, { width: 200, height: 120 });
win.addEventListener('message', (ev) => { inbox.push(ev.data); });

let loaded = 0;
win.addEventListener('load', () => { loaded++; });
flush();
assert(loaded === 1, 'child window loaded');

// ---- baseline: light, no events yet ----------------------------------------
let state = ask(win, inbox);
assert(state.matches === false,
       'child sees prefers-color-scheme: dark = false under the light scheme');
assert(state.changes.length === 0,
       'no change events before any flip, got ' + state.changes.length);

// ---- light → dark: the live read AND the event must both move --------------
bro.settings.set('appearance.colorScheme', 'dark');
flush();

state = ask(win, inbox);
assert(state.matches === true,
       'scheme flip reaches the secondary window realm (.matches went true)');
assert(state.changes.length === 1,
       "exactly one 'change' delivered to the secondary window, got " +
       state.changes.length);
assert(state.changes[0].matches === true,
       'the delivered change observed the new scheme');
assert(state.changes[0].evMatches === true,
       'the change event object carries matches:true');

// ---- dark → light: fires again, and in the right direction -----------------
bro.settings.set('appearance.colorScheme', 'light');
flush();

state = ask(win, inbox);
assert(state.matches === false, 'flipping back updates the live read');
assert(state.changes.length === 2,
       'a second change was delivered, got ' + state.changes.length);
assert(state.changes[1].matches === false,
       'the second change observed the light scheme');

// ---- a no-op set must not manufacture an event -----------------------------
bro.settings.set('appearance.colorScheme', 'light');
flush();

state = ask(win, inbox);
assert(state.changes.length === 2,
       'setting the same scheme again delivers nothing new, got ' +
       state.changes.length);

win.close();
flush();
assert(win.closed === true, 'child window closed');

console.log('PASS test_multiwindow_matchmedia');

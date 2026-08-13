// The driver for the events probe: bro-headless's own script surface
// (docs/headless.md), pointed at a bronze-compiled app.
//
// Every line it prints is prefixed `DRV `, the page script's are `PAGE `, and
// the compiled app's are `APP `. run_events_test.sh pins all three — see the
// note there about why they are compared as two blocks rather than one
// interleaved stream.
//
// The order below is the order the checks build on each other: real input
// first (it is the thing that was broken), then the custom-event channel, then
// the two propagation controls.

// One frame so the app's first rAF has run and the layout the click will hit
// test against is settled.
advanceTime(16);

const canvas = document.querySelector('canvas');
console.log('DRV canvasInDocument=' + (canvas !== null));

// --- Real input, through the whole pipeline -------------------------------
// The canvas is absolutely positioned at the viewport origin at 200x100 (the
// appdir's stylesheet), so these coordinates are inside it and the offsets
// they produce are the coordinates themselves.
click(20, 30);
mouseMove(40, 50);
wheel(60, 70, 12, 0);

// keydown has no element under a cursor to aim at: with nothing focused it
// reaches the document element, which is where the compiled app's
// `document.addEventListener('keydown')` registration actually lives. 97 is
// SDL's 'a'; 1 is KMOD_LSHIFT.
keyDown(97, 0, 1);

// --- The custom-event channel, both directions ----------------------------
// The driver only kicks it off. pageSend is the INTERPRETED page script's
// function: it dispatches `page:toApp`, the compiled app answers on
// `app:toPage`, and the page script prints what came back. Two boundary
// crossings, neither of which moves a heap value.
globalThis.pageSend('one');

// The driver watching the same channel from its own listener — the compiled
// app's answer is an ordinary DOM event, so anything in the realm can hear it.
document.addEventListener('app:toPage', function (e) {
    console.log('DRV heard=' + e.detail);
});
globalThis.pageSend('two');

// --- preventDefault, compiled listener -> dispatcher ----------------------
// The compiled handler for `app:cancelme` calls preventDefault; dispatchEvent
// answers false only if that reached the dom::Event dispatch is walking with.
const notCancelled = document.dispatchEvent(new CustomEvent('app:cancelme', {
    detail: 'stop', bubbles: true, cancelable: true
}));
console.log('DRV cancelled=' + (notCancelled === false));

// --- stopPropagation, compiled listener -> the rest of the walk -----------
// Dispatched AT THE CANVAS and bubbling. The compiled canvas listener stops
// it, so the compiled document listener for the same type must not print.
canvas.dispatchEvent(new CustomEvent('app:stophere', {
    detail: 'here', bubbles: true, cancelable: true
}));

// --- The window path, which already worked, as a control ------------------
window.dispatchEvent(new CustomEvent('app:toWindow', { detail: 'w' }));

// A trailing frame: nothing above scheduled work, so nothing new may appear.
// A line printed after this one is an event that fired late, which is a
// finding, not noise.
advanceTime(16);
console.log('DRV done=1');

// A modal <dialog> lives in the top layer (dom::Document::topLayer):
//   - it paints above everything — a z-index:99999 fixed box, an ancestor
//     with overflow:hidden and a transform — over its ::backdrop;
//   - the UA sheet makes it :modal, fixed and centred in the viewport;
//   - everything outside it is inert: clicks land on the dialog (the
//     backdrop's target), focus() and Tab stay inside;
//   - Escape fires a cancelable `cancel`, then closes;
//   - close(), losing `open`, or leaving the document takes it out again.

const root = document.getElementById('root');
root.innerHTML = `
<style>
  #outside { position: absolute; left: 10px; top: 10px; width: 80px; height: 30px; }
  #clip { position: relative; z-index: 1; overflow: hidden; width: 40px; height: 40px;
          transform: translateX(0px); margin-top: 60px; }
  #cover { position: fixed; z-index: 99999; pointer-events: none;
           left: calc(50% - 60px); top: calc(50% - 30px); width: 120px; height: 60px;
           background: rgb(255, 0, 0); }
  /* The test page resets every margin to 0 (style.css), which beats the UA
     sheet's margin:auto — exactly as a reset sheet does in a browser — so the
     dialogs ask for their centring back. */
  #dlg { background: rgb(0, 255, 0); width: 200px; height: 100px; padding: 0; border: none;
         margin: auto; }
  #dlg::backdrop { background: rgb(0, 0, 255); }
  #dlg button { width: 80px; height: 30px; }
  #dlg2 { background: rgb(255, 255, 0); width: 60px; height: 40px; padding: 0; border: none;
          margin: auto; }
</style>
<button id="outside">outside</button>
<div id="clip"><dialog id="dlg"><button id="one">one</button><button id="two">two</button></dialog></div>
<div id="cover"></div>
<dialog id="dlg2"><button id="three">three</button></dialog>`;
flush();

const dlg = document.getElementById('dlg');
const dlg2 = document.getElementById('dlg2');
const outside = document.getElementById('outside');
const one = document.getElementById('one');
const two = document.getElementById('two');
const vw = innerWidth, vh = innerHeight;
const cx = Math.floor(vw / 2), cy = Math.floor(vh / 2);
const corner = [20, cy + 150];   // clear of the dialogs and of the page's controls

function px(x, y) { return getPixel(x, y); }
function isColor(p, r, g, b) {
    return Math.abs(p.r - r) < 30 && Math.abs(p.g - g) < 30 && Math.abs(p.b - b) < 30;
}
function center(el) {
    const r = el.getBoundingClientRect();
    return [r.left + r.width / 2, r.top + r.height / 2];
}
const clicks = [];
for (const el of [outside, dlg, one, two]) {
    el.addEventListener('click', (e) => clicks.push(el.id + (e.target === el ? '' : '<' + e.target.id)));
}

// ---- before: the cover is on top, nothing is modal ------------------------------
assert(!dlg.matches(':modal'), 'closed dialog is not :modal');
assert(isColor(px(cx, cy), 255, 0, 0), 'the z-index cover paints at the centre, got ' +
       JSON.stringify(px(cx, cy)));
outside.focus();
assert(document.activeElement === outside, 'outside button focused before opening');

// ---- showModal: top layer --------------------------------------------------------
dlg.showModal();
flush();
assert(dlg.open && dlg.matches(':modal'), 'showModal makes the dialog :modal');
assert(!dlg2.matches(':modal'), 'another dialog is not :modal');
assert(getComputedStyle(dlg).position === 'fixed', 'a modal dialog is position:fixed, got ' +
       getComputedStyle(dlg).position);

const r = dlg.getBoundingClientRect();
assert(Math.abs(r.width - 200) < 1 && Math.abs(r.height - 100) < 1,
       'dialog box is its own size, got ' + r.width + 'x' + r.height);
assert(Math.abs(r.left + r.width / 2 - vw / 2) <= 2 && Math.abs(r.top + r.height / 2 - vh / 2) <= 2,
       'modal dialog is centred in the viewport: ' + JSON.stringify([r.left, r.top, vw, vh]));

// Inside the red z-index:99999 cover, below the dialog's buttons.
const onDialog = px(cx, cy + 20);
assert(isColor(onDialog, 0, 255, 0),
       'dialog paints above the z-index:99999 cover and outside its clipping ancestor, got ' +
       JSON.stringify(onDialog));
assert(isColor(px(corner[0], corner[1]), 0, 0, 255),
       'the styled ::backdrop covers the page, got ' + JSON.stringify(px(corner[0], corner[1])));
assert(isColor(px(50, 25), 0, 0, 255), 'the backdrop covers the outside button too');

// ---- focus moved inside; the page outside is inert ----------------------------------
assert(document.activeElement === one, 'the dialog focusing steps focus its first control, got ' +
       (document.activeElement && document.activeElement.id));
outside.focus();
assert(document.activeElement === one, 'focus() on an inert element does nothing');

const [ox, oy] = center(outside);
assert(document.elementFromPoint(ox, oy) === dlg,
       'a point outside the dialog hits the dialog (its backdrop), got ' +
       (document.elementFromPoint(ox, oy) || {}).id);
const [tx, ty] = center(two);
assert(document.elementFromPoint(tx, ty) === two, 'a control inside the dialog is hit');

clicks.length = 0;
click(ox, oy);
assert(clicks.indexOf('outside') < 0, 'a click over the inert page never reaches it: ' + clicks);
assert(clicks.indexOf('dlg') >= 0, 'the backdrop click targets the dialog: ' + clicks);
clicks.length = 0;
click(tx, ty);
assert(clicks.indexOf('two') >= 0, 'a click inside the dialog works: ' + clicks);

// Tab cycles inside the dialog only.
one.focus();
keyDown(9); keyUp(9);
assert(document.activeElement === two, 'Tab → second dialog control');
keyDown(9); keyUp(9);
assert(document.activeElement === one, 'Tab wraps within the dialog, never to the page, got ' +
       (document.activeElement && document.activeElement.id));

// ---- Escape: cancel (cancelable), then close -----------------------------------------
const events = [];
let preventCancel = true;
dlg.addEventListener('cancel', (e) => { events.push('cancel'); if (preventCancel) e.preventDefault(); });
dlg.addEventListener('close', () => events.push('close'));
keyDown(27); keyUp(27);
advanceTime(20); flush();
assert(events.join() === 'cancel', 'Escape fires cancel first: ' + events);
assert(dlg.open && dlg.matches(':modal'), 'a cancelled cancel keeps the dialog open');

preventCancel = false;
events.length = 0;
keyDown(27); keyUp(27);
assert(!dlg.open, 'Escape closed the dialog');
advanceTime(20); flush();
assert(events.join() === 'cancel,close', 'cancel then close: ' + events);
assert(!dlg.matches(':modal'), 'a closed dialog leaves the top layer');
assert(document.activeElement === outside, 'focus returns to the element focused before, got ' +
       (document.activeElement && document.activeElement.id));
assert(isColor(px(cx, cy), 255, 0, 0), 'the page paints again after close, got ' +
       JSON.stringify(px(cx, cy)));
assert(!isColor(px(corner[0], corner[1]), 0, 0, 255), 'the backdrop is gone');
clicks.length = 0;
click(ox, oy);
assert(clicks.indexOf('outside') >= 0, 'the page is interactive again: ' + clicks);

// ---- close() -----------------------------------------------------------------------------
dlg.showModal();
flush();
assert(dlg.matches(':modal'), 're-opened');
dlg.close('bye');
flush();
assert(!dlg.matches(':modal') && dlg.returnValue === 'bye', 'close() leaves the top layer');
assert(document.elementFromPoint(ox, oy) === outside, 'and the page is hit again');

// ---- losing `open` by hand, or leaving the document ------------------------------------------
dlg.showModal();
flush();
dlg.removeAttribute('open');
flush();
assert(!dlg.matches(':modal'), 'removing [open] takes the dialog out of the top layer');
assert(document.elementFromPoint(ox, oy) === outside, 'page hit after [open] removed');

dlg.showModal();
flush();
const clip = document.getElementById('clip');
clip.removeChild(dlg);
flush();
assert(!dlg.matches(':modal'), 'a dialog removed from the document leaves the top layer');
assert(document.elementFromPoint(ox, oy) === outside, 'page hit after the dialog was removed');
clip.appendChild(dlg);
if (dlg.open) dlg.close();
flush();

// ---- two modal dialogs: the later one is on top; Escape closes it first ------------------------
dlg.showModal();
dlg2.showModal();
flush();
assert(dlg.matches(':modal') && dlg2.matches(':modal'), 'both modal');
const r2 = dlg2.getBoundingClientRect();
assert(isColor(px(Math.floor(r2.left + 5), Math.floor(r2.top + 35)), 255, 255, 0),
       'the second dialog paints above the first');
assert(document.activeElement === document.getElementById('three'), 'focus in the top dialog');
one.focus();
assert(document.activeElement === document.getElementById('three'),
       'the lower modal dialog is inert under the upper one');
keyDown(27); keyUp(27);
assert(!dlg2.open && dlg.open, 'Escape closes the topmost dialog only');
keyDown(27); keyUp(27);
assert(!dlg.open, 'a second Escape closes the next');
advanceTime(20); flush();

// ---- compositor layers stay under the top layer ------------------------------------------------
// A canvas is its own composited texture, and a transform animation is
// promoted to a layer drawn over the base; neither may cover a modal dialog.
root.innerHTML = `
<style>
  #cv { position: fixed; left: ${cx - 150}px; top: ${cy - 100}px; z-index: 50; }
  #spin { position: fixed; left: ${cx - 40}px; top: ${cy + 10}px; width: 80px; height: 30px;
          z-index: 60; background: rgb(255, 0, 255); animation: nudge 1s linear infinite; }
  @keyframes nudge { from { transform: translateX(0px); } to { transform: translateX(1px); } }
  #plain { margin: auto; width: 200px; height: 100px; padding: 0; border: none;
           background: rgb(0, 255, 0); }
</style>
<canvas id="cv" width="300" height="200"></canvas><div id="spin"></div><dialog id="plain"></dialog>`;
const g = document.getElementById('cv').getContext('2d');
g.fillStyle = 'rgb(255, 0, 0)';
g.fillRect(0, 0, 300, 200);
advanceTime(20); flush();
assert(isColor(px(cx, cy - 20), 255, 0, 0), 'the canvas paints before the dialog opens');
document.getElementById('plain').showModal();
advanceTime(20); flush(); advanceTime(20); flush();
assert(isColor(px(cx, cy - 20), 0, 255, 0), 'the dialog paints over a canvas layer, got ' +
       JSON.stringify(px(cx, cy - 20)));
assert(isColor(px(cx, cy + 25), 0, 255, 0), 'the dialog paints over an animating element, got ' +
       JSON.stringify(px(cx, cy + 25)));
const dimmed = px(cx - 140, cy - 90);
assert(dimmed.r > 200 && dimmed.r < 250 && dimmed.g < 20,
       'the default ::backdrop tints the canvas outside the dialog, got ' + JSON.stringify(dimmed));
document.getElementById('plain').close();
advanceTime(20); flush();

root.innerHTML = '';
console.log('dialog top layer: OK');

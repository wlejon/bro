// HTMLDialogElement: open / returnValue / show() / showModal() / close() /
// requestClose() and the close / cancel events, plus the UA rendering rule
// that a closed dialog is not rendered.

const d = document.createElement('dialog');
d.innerHTML = '<p id="dlg-body">hello</p>';
document.body.appendChild(d);
flush();

assert(d instanceof HTMLDialogElement, 'a <dialog> is an HTMLDialogElement');
assert(d instanceof HTMLElement, 'and an HTMLElement');
for (const m of ['show', 'showModal', 'close', 'requestClose']) {
    assert(typeof d[m] === 'function', m + ' exists');
}
assert(d.open === false, 'closed by default');
assert(d.returnValue === '', 'returnValue starts empty');

// Closed: not rendered.
assert(getComputedStyle(d).display === 'none', 'a closed dialog is display:none');
assert(d.getBoundingClientRect().width === 0, 'and has no box');

// show(): open, rendered, attribute reflected.
d.show();
flush();
assert(d.open === true, 'show() opens');
assert(d.hasAttribute('open'), 'open is reflected as the attribute');
assert(getComputedStyle(d).display === 'block', 'an open dialog is display:block');
assert(d.getBoundingClientRect().width > 0, 'an open dialog lays out');
d.show();   // already open non-modally: a no-op
assert(d.open === true, 'show() twice is a no-op');

// showModal() on a non-modal open dialog is an InvalidStateError.
let err = null;
try { d.showModal(); } catch (e) { err = e; }
assert(err && err.name === 'InvalidStateError', 'showModal on a non-modal open dialog throws');

// close(result): closes, sets returnValue, fires `close` as a later task.
const closes = [];
d.addEventListener('close', (e) => closes.push(e));
let onclosed = 0;
d.onclose = () => { onclosed++; };
d.close('ok');
assert(d.open === false, 'close() closes');
assert(!d.hasAttribute('open'), 'the attribute is gone');
assert(d.returnValue === 'ok', 'close(result) sets returnValue');
assert(closes.length === 0, 'close event is not synchronous');
advanceTime(20);
assert(closes.length === 1, 'one close event, got ' + closes.length);
assert(onclosed === 1, 'onclose ran');
assert(closes[0].type === 'close', 'type close');
assert(closes[0].bubbles === false && closes[0].cancelable === false, 'close is simple');
flush();
assert(getComputedStyle(d).display === 'none', 'closed again: not rendered');

// close() on a closed dialog does nothing (no event).
d.close('ignored');
advanceTime(20);
assert(closes.length === 1, 'closing a closed dialog fires nothing');
assert(d.returnValue === 'ok', 'and leaves returnValue alone');

// close() with no argument keeps the previous returnValue.
d.show();
d.close();
advanceTime(20);
assert(d.returnValue === 'ok', 'close() without a result keeps returnValue');
d.returnValue = 'set';
assert(d.returnValue === 'set', 'returnValue is writable');

// showModal(): open and modal; show() on a modal dialog throws.
d.showModal();
assert(d.open === true, 'showModal() opens');
err = null;
try { d.show(); } catch (e) { err = e; }
assert(err && err.name === 'InvalidStateError', 'show on a modal dialog throws');
d.showModal();   // already modal: a no-op
d.close('modal-done');
advanceTime(20);
assert(d.returnValue === 'modal-done', 'modal close sets returnValue');

// showModal() on a disconnected dialog throws.
const loose = document.createElement('dialog');
err = null;
try { loose.showModal(); } catch (e) { err = e; }
assert(err && err.name === 'InvalidStateError', 'showModal on a disconnected dialog throws');
assert(loose.open === false, 'and leaves it closed');

// requestClose(): a cancelable `cancel` first; preventDefault keeps it open.
d.show();
let cancels = 0;
const veto = (e) => { cancels++; e.preventDefault(); };
d.addEventListener('cancel', veto);
d.requestClose('nope');
assert(cancels === 1, 'cancel fired');
assert(d.open === true, 'a prevented cancel keeps the dialog open');
d.removeEventListener('cancel', veto);
d.requestClose('yes');
assert(d.open === false, 'an unprevented requestClose closes');
assert(d.returnValue === 'yes', 'with its return value');

// The open setter.
d.open = true;
assert(d.hasAttribute('open'), 'open = true sets the attribute');
d.open = false;
assert(!d.hasAttribute('open'), 'open = false removes it');

// Parsed <dialog open> starts open.
const holder = document.createElement('div');
holder.innerHTML = '<dialog open id="pre">x</dialog>';
document.body.appendChild(holder);
flush();
const pre = document.getElementById('pre');
assert(pre instanceof HTMLDialogElement, 'parsed dialog is an HTMLDialogElement');
assert(pre.open === true, 'parsed <dialog open> is open');

console.log('dialog element: OK');

// Regression: removing the focused element must not leave the document's
// activeElement dangling. A button whose click handler removes its own row
// is focused by the very mousedown that triggers it; the engine then polls
// document_->activeElement() on every subsequent mouse event (e.g. the
// range-slider drag check in handleMouseMove) — pre-fix that dereferenced
// the freed element and crashed (listen-lab's template × button).

const root = document.getElementById('root');
assert(root !== null, 'root element exists');

// A self-removing button, like listen-lab's per-template × remove button.
const row = document.createElement('div');
const btn = document.createElement('button');
btn.id = 'rm';
btn.textContent = 'x';
btn.addEventListener('click', () => { row.remove(); });
row.appendChild(btn);
root.appendChild(row);
flush();

// Real input path: mousedown focuses the button, mouseup completes the click,
// the handler removes the row (and the focused button with it).
const r = btn.getBoundingClientRect();
const cx = r.x + r.width / 2, cy = r.y + r.height / 2;
click(cx, cy);
flush();   // layout + drainPendingFrees — the button's memory is gone now
assert(document.querySelector('#rm') === null, 'button removed by its own click');

// Pre-fix: handleMouseMove read the freed element through activeElement().
mouseMove(cx + 5, cy + 5);
mouseMove(cx + 10, cy + 2);
flush();

assert(document.activeElement !== null, 'activeElement falls back, never dangles');
assert(document.activeElement.tagName !== 'BUTTON',
       'removed button is not the active element');

// Same hole via the synthetic-click path (dispatchClickOn also focuses).
const row2 = document.createElement('div');
const btn2 = document.createElement('button');
btn2.id = 'rm2';
btn2.textContent = 'x';
btn2.addEventListener('click', () => { row2.remove(); });
row2.appendChild(btn2);
root.appendChild(row2);
flush();
btn2.click();
flush();
mouseMove(cx + 3, cy + 7);
flush();
assert(document.activeElement === null || document.activeElement.id !== 'rm2',
       'synthetically clicked + removed button is not the active element');

console.log('PASS');

// Run by test_menu_startup_layout.js in a child bro-headless, against both
// menu_startup_app/ (bro.menu.show() synchronously at startup) and
// menu_startup_defer_app/ (the same call one task later). Prints one
// MEASURE_JSON line the parent compares between the two runs, and asserts the
// CSS-specified boxes here so a failure names the element that is wrong.

// Let the deferred app's timer run, so both documents have the menu bar up and
// have settled; the control boxes must not depend on either.
advanceTime(16);
flush();

assert(bro.menu.visible === true, 'the menu bar is showing in both apps');

const ids = ['d', 't', 'e', 'n', 's', 'a', 'b'];
const box = {};
for (const id of ids) {
  const el = document.getElementById(id);
  assert(el, 'element #' + id + ' exists');
  const r = el.getBoundingClientRect();
  box[id] = { w: r.width, h: r.height, ow: el.offsetWidth, oh: el.offsetHeight };
}

const near = (a, b) => Math.abs(a - b) < 0.5;
const show = (id) => id + ' = ' + box[id].w.toFixed(2) + ' x ' + box[id].h.toFixed(2);

// A plain styled div: 300 + 2*5 padding + 2*2 border = 314 by 40 + 10 + 4 = 54.
assert(near(box.d.w, 314) && near(box.d.h, 54), 'styled div box: ' + show('d'));

// The form controls. Width comes straight from CSS (content-box), so it is
// exact; height is one line of text plus padding and border, so the assertion
// is that the line is THERE — the bug collapsed these to padding + border
// (14px for the inputs and the select), losing the content row entirely.
for (const id of ['t', 'e']) {
  assert(near(box[id].w, 420 + 18 + 2), 'text input width: ' + show(id));
  assert(box[id].h > 14 + 8,
         'text input keeps a line of content height: ' + show(id));
}
assert(near(box.n.w, 90 + 18 + 2), 'number input width: ' + show('n'));
assert(box.n.h > 14 + 8, 'number input keeps a line of content height: ' + show('n'));
// <select> is border-box in the UA sheet, so its CSS width IS its border box.
assert(near(box.s.w, 220), 'select width: ' + show('s'));
assert(box.s.h > 14 + 8, 'select keeps a line of content height: ' + show('s'));

// textarea and button size from CSS alone, so they were never affected — they
// are here to prove the fix does not move anything that was already right.
assert(near(box.a.w, 200 + 12 + 2) && near(box.a.h, 60 + 12 + 2),
       'textarea box: ' + show('a'));
assert(box.b.h > 14 + 8, 'button keeps a line of content height: ' + show('b'));

console.log('MEASURE_JSON ' + JSON.stringify(box));

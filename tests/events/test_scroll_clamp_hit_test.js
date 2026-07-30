// A scroller whose content shrinks must re-clamp its scroll offset, so hit
// testing keeps landing where the pixels are.
//
// The bug: one overflow:auto scroller with tab-panel children toggled by
// display:none. Scroll while the tall panel is active, switch to a short one,
// and the stored scrollTop stays at the tall panel's offset — past the new max.
// The paint path clamps on read, so the frame looks right, but hit testing read
// the raw value and shifted every child up by the difference: clicks landed on
// nothing. The wheel couldn't heal it either, because a scroller whose content
// now fits (maxScrollTop == 0) is skipped by the wheel handler entirely, so the
// stale offset survived until the tall panel came back and was scrolled to 0.

const root = document.getElementById('root');

root.innerHTML =
  '<div id="scroller" style="position:absolute;left:0;top:0;width:200px;height:100px;overflow-y:auto;">' +
  '  <div id="tall" style="height:600px;">' +
  '    <div id="tall-target" style="height:40px;">tall</div>' +
  '  </div>' +
  '  <div id="short" style="display:none;">' +
  '    <div id="short-target" style="height:40px;">short</div>' +
  '  </div>' +
  '</div>';
flush();

const scroller = document.getElementById('scroller');
const shortEl = document.getElementById('short');
const tallEl = document.getElementById('tall');
const shortTarget = document.getElementById('short-target');

// Scroll the tall panel well down — a valid offset for 600px of content.
scroller.scrollTop = 400;
flush();
assert(scroller.scrollTop === 400, 'tall panel scrolled to 400 (got ' + scroller.scrollTop + ')');

// Switch panels: the scroller's content is now 40px, which fits the 100px box,
// so the maximum valid offset is 0.
tallEl.style.display = 'none';
shortEl.style.display = 'block';
flush();

assert(scroller.scrollTop === 0,
       'offset re-clamped when content shrank below the viewport (got ' + scroller.scrollTop + ')');

// The real symptom: the short panel's control has to be clickable where it is
// drawn. Its rect comes from the geometry path (which always clamped); the click
// goes through hit testing (which did not).
const r = shortTarget.getBoundingClientRect();
assert(r.height > 0, 'short target has a box (' + r.width + '×' + r.height + ')');

let hits = 0;
shortTarget.addEventListener('click', () => { hits++; });
click(r.left + r.width / 2, r.top + r.height / 2);
assert(hits === 1, 'click at the short target\'s own rect reached it (got ' + hits + ' hits)');

// The click must have resolved to the target itself, not to whatever the stale
// offset would have slid under the cursor.
let lastTarget = null;
shortTarget.addEventListener('click', (e) => { lastTarget = e.target; });
click(r.left + r.width / 2, r.top + r.height / 2);
assert(lastTarget === shortTarget,
       'hit test resolved to the painted element (got ' +
       (lastTarget ? lastTarget.id || lastTarget.tagName : 'null') + ')');

// Switching back must not resurrect the old offset — it was clamped, not stashed.
shortEl.style.display = 'none';
tallEl.style.display = 'block';
flush();
assert(scroller.scrollTop === 0, 'tall panel returns at the clamped offset (got ' + scroller.scrollTop + ')');

// A partial shrink clamps to the new max rather than to 0.
scroller.scrollTop = 500;
flush();
assert(scroller.scrollTop === 500, 'scrolled to 500 (got ' + scroller.scrollTop + ')');
document.getElementById('tall').style.height = '300px';
flush();
assert(scroller.scrollTop === 200,
       'offset clamped to the new max (300 content - 100 view = 200), got ' + scroller.scrollTop);

// A scroll event fires for the involuntary move, as it does in browsers.
scroller.style.height = '100px';
let scrolls = 0;
scroller.addEventListener('scroll', () => { scrolls++; });
document.getElementById('tall').style.height = '150px';
flush();
assert(scroller.scrollTop === 50, 'clamped again to 50 (got ' + scroller.scrollTop + ')');
assert(scrolls >= 1, 'a scroll event fired for the re-clamp (got ' + scrolls + ')');

root.innerHTML = '';

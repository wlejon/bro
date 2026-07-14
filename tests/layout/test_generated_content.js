// ::before / ::after generated content.
//
// Generated content is resolved incrementally — only for the elements whose
// style was re-resolved this pass — because the old whole-document pass cost
// ~3.5 ms on every pointer move across a long list. That is only sound while a
// pseudo-element is a pure function of its originating element, which counter()
// and the quote keywords are NOT: they read state that accumulates across the
// document in tree order, so those fall back to the full in-order walk.
//
// A pseudo appearing or disappearing is also a geometry change — the layout
// tree grows or drops a synthetic box for it — so it has to promote the frame
// to a real layout. Without that, a :hover::before never shows up at all
// (hover restyles are paint-only by design).
//
// So this covers all of it: local content on the fast path, stateful content on
// the slow one, and hover-driven content, which needs both to be right.

const root = document.getElementById('root');
const style = document.createElement('style');
document.head.appendChild(style);
style.textContent = `
  #root { font: 16px monospace; }
  .plain::before { content: ""; }
  .wide::before  { content: "XXXXXXXXXXXXXXXX"; }
  .attrp::before { content: attr(data-n); }
  .hov::before   { content: ""; }
  .hov:hover::before { content: "XXXXXXXXXXXXXXXX"; }
  .counted > li { display: inline-block; list-style: none; }
  .counted > li::before { content: counter(item) ". "; counter-increment: item; }
`;
flush();

const w = (id) => document.getElementById(id).getBoundingClientRect().width;

// --- literal content reaches layout --------------------------------------
root.innerHTML = '<span id="a" class="plain">t</span><br>' +
                 '<span id="b" class="wide">t</span>';
flush();
assert(w('b') > w('a') + 50,
       '::before literal widens the box (' + w('a') + ' -> ' + w('b') + ')');

// --- an element added after load gets its pseudo --------------------------
const fresh = document.createElement('span');
fresh.id = 'f';
fresh.className = 'wide';
fresh.textContent = 't';
root.appendChild(fresh);
flush();
assert(Math.abs(w('f') - w('b')) < 1,
       'a newly-appended element gets its ::before (' + w('f') + ')');

// --- attr() re-resolves when the attribute changes ------------------------
// The attribute write dirties the element, so the incremental pass must pick it
// up. If it stops doing so, a data-driven ::before silently freezes.
root.innerHTML = '<span id="c" class="attrp" data-n="1">t</span>';
flush();
const narrow = w('c');
document.getElementById('c').setAttribute('data-n', 'XXXXXXXXXXXXXXXX');
flush();
assert(w('c') > narrow + 50,
       '::before attr() re-resolves on attribute change (' + narrow + ' -> ' + w('c') + ')');

// --- :hover — the case the incremental path exists for --------------------
// A hover restyle is paint-only, so this only works if a pseudo whose content
// changed promotes the frame to a layout. Both directions.
root.innerHTML = '<span id="e" class="hov">t</span>';
flush();
const off = w('e');
const r = document.getElementById('e').getBoundingClientRect();
mouseMove(r.x + r.width / 2, r.y + r.height / 2);
flush();
const on = w('e');
assert(on > off + 50, ':hover::before appears on hover (' + off + ' -> ' + on + ')');
mouseMove(r.x + 400, r.y + 400);
flush();
assert(w('e') === off,
       ':hover::before goes away again (' + on + ' -> ' + w('e') + ', want ' + off + ')');

// --- counters still count, in document order ------------------------------
// The whole reason the full walk survives. Each <li> shrink-wraps, so its width
// is marker + text: the tenth is "10. t" and the first is "1. t". A per-element
// resolve cannot produce this — it has no idea how many <li> came before.
let items = '';
for (let i = 0; i < 10; i++) items += '<li id="li' + i + '">t</li>';
root.innerHTML = '<ol class="counted">' + items + '</ol>';
flush();
assert(w('li9') > w('li0'),
       'counter() increments in document order — "10. " is wider than "1. " (' +
       w('li0') + ' -> ' + w('li9') + ')');

// --- quotes: the UA sheet's q::before { content: open-quote } -------------
root.innerHTML = '<span id="q1">text</span><q id="q2">text</q>';
flush();
assert(w('q2') > w('q1'), '<q> gets its quote marks (' + w('q1') + ' -> ' + w('q2') + ')');

console.log('PASS: generated content — literal, attr(), hover, counters, quotes');

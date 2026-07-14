// Flex items must be reusable across layout passes.
//
// A flex container lays an auto-basis item out twice per pass (measure, then
// final). The incremental machinery used to poison the item's cache on the
// second visit, so any subtree that had ever been under a re-laying flex
// container could never be reused again — one leaf change re-laid the whole
// document, every pass, forever. Now the re-visit re-keys the cache to the
// final call's inputs, the measure is skipped for clean items, and each item
// remembers its natural cross size so a reused (stretch-written) box doesn't
// feed the previous line height back into the next line calculation.

const root = document.getElementById('root');

const sheet = document.createElement('style');
sheet.textContent = `
  #fr-app { width: 600px; font: 14px Arial; }
  #fr-app .row { display: flex; align-items: center; gap: 8px; padding: 2px; }
  #fr-app .row .grow { flex: 1; }
  #fr-col { display: flex; flex-direction: column; width: 300px; }
  #fr-stretch { display: flex; width: 400px; }
  #fr-stretch > div { flex: none; width: 100px; }
`;
document.head.appendChild(sheet);

root.innerHTML = `
  <div id="fr-app">
    <div id="fr-rows"></div>
    <div id="fr-col">
      <div>alpha</div><div id="fr-col-b">beta</div><div>gamma</div>
    </div>
    <div id="fr-stretch">
      <div id="fr-s1">short</div>
      <div id="fr-s2">tall</div>
    </div>
  </div>`;
const rows = document.getElementById('fr-rows');
for (let i = 0; i < 200; i++) {
  const row = document.createElement('div');
  row.className = 'row';
  row.innerHTML = '<span class="lbl">row ' + i + '</span>' +
                  '<span class="grow">value</span><button>x</button>';
  rows.appendChild(row);
}
flush();

const el = (id) => document.getElementById(id);
const h = (id) => el(id).getBoundingClientRect().height;

// ── scope: a leaf change under one flex row must not re-lay the document ──
const target = rows.children[100].querySelector('.grow');
target.textContent = 'warm';
flush();
perf.reset();
target.textContent = 'changed';
flush();
let p = perf.stats();
assert(p.nodesLaidOut < 60,
       'leaf change in one flex row stays O(change): ' + p.nodesLaidOut +
       ' nodes laid out (' + p.nodesReused + ' reused)');

// The same pass must actually reuse something (the machinery is live, not
// just quiet): sibling rows on the dirty container's path hand back caches.
assert(p.nodesReused > 0, 'flex siblings were reused: ' + p.nodesReused);

// ── correctness: geometry still responds to changes ──
const rowH = rows.children[50].getBoundingClientRect().height;
rows.children[50].querySelector('.lbl').style.fontSize = '40px';
flush();
assert(rows.children[50].getBoundingClientRect().height > rowH + 10,
       'row grows when its label grows');
rows.children[50].querySelector('.lbl').style.fontSize = '';
flush();
assert(Math.abs(rows.children[50].getBoundingClientRect().height - rowH) < 0.5,
       'row shrinks back');

// Stretch line-height shrink: make one item tall, its stretch sibling must
// grow; make it short again, the sibling must SHRINK back (a reused box that
// reported its stretched size as natural would keep the line tall forever).
const s1Before = h('fr-s1');
el('fr-s2').style.height = '150px';
flush();
assert(Math.abs(h('fr-s1') - 150) < 0.5, 's1 stretches to tall sibling: ' + h('fr-s1'));
el('fr-s2').style.height = '';
flush();
assert(Math.abs(h('fr-s1') - s1Before) < 0.5,
       's1 shrinks back when sibling shrinks: ' + h('fr-s1') + ' vs ' + s1Before);

// Column flex: content change re-measures through the cached measure scalar.
const colB = el('fr-col-b');
const colH = h('fr-col');
colB.textContent = 'beta beta beta beta beta beta beta beta beta beta beta ' +
                   'beta beta beta beta beta beta beta beta beta beta beta';
flush();
assert(h('fr-col') > colH, 'column grows when an item wraps: ' + h('fr-col'));
colB.textContent = 'beta';
flush();
assert(Math.abs(h('fr-col') - colH) < 0.5, 'column shrinks back: ' + h('fr-col'));

root.innerHTML = '';

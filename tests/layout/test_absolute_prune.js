// The absolute-elements pass prunes branches with no positioned elements
// (LayoutNode::subtreeHasPositioned). Two things have to hold:
//
//   - correctness: a positioned box still re-resolves against its containing
//     block's CURRENT geometry every pass — the containing block can move
//     because of a change far away, with nothing in the positioned box's own
//     branch dirty. And position: toggling in/out of absolute must flip a
//     branch's "nothing positioned here" verdict.
//   - scope: a document full of abs-free branches doesn't pay two style
//     lookups per element per pass just to rediscover that.

const root = document.getElementById('root');

const sheet = document.createElement('style');
sheet.textContent = `
  #ap-app { width: 600px; font: 16px Arial; }
  #ap-spacer { height: 50px; }
  #ap-holder { position: relative; height: 80px; }
  #ap-badge { position: absolute; right: 10px; top: 5px; width: 30px; height: 20px; }
  #ap-fixed { position: fixed; left: 4px; top: 4px; width: 10px; height: 10px; }
  #ap-toggle { width: 40px; height: 12px; }
  #ap-rows div { height: 20px; }
`;
document.head.appendChild(sheet);

root.innerHTML = `
  <div id="ap-app">
    <div id="ap-spacer"></div>
    <div id="ap-holder"><div id="ap-badge"></div></div>
    <div id="ap-toggle"></div>
    <div id="ap-fixed"></div>
    <div id="ap-rows"></div>
  </div>`;
const rows = document.getElementById('ap-rows');
for (let i = 0; i < 100; i++) {
  const d = document.createElement('div');
  d.textContent = 'row ' + i;
  rows.appendChild(d);
}
flush();

const el = (id) => document.getElementById(id);
const top = (id) => el(id).getBoundingClientRect().top;
const rootTop = root.getBoundingClientRect().top;

assert(Math.abs(top('ap-badge') - (rootTop + 55)) < 0.5,
       'baseline badge top: ' + top('ap-badge'));

// ── correctness ─────────────────────────────────────────────────────────────

// Move the containing block by growing the spacer above it. #ap-holder and
// #ap-badge styles are untouched; the badge must follow anyway.
el('ap-spacer').style.height = '150px';
flush();
assert(Math.abs(top('ap-badge') - (rootTop + 155)) < 0.5,
       'badge follows moved containing block: ' + top('ap-badge'));

// Fixed stays viewport-anchored across unrelated relayouts.
const fr = el('ap-fixed').getBoundingClientRect();
assert(Math.abs(fr.left - 4) < 0.5 && Math.abs(fr.top - 4) < 0.5,
       'fixed anchored: ' + fr.left + ',' + fr.top);

// A static element becoming absolute in a previously abs-free branch must be
// discovered by the next pass.
el('ap-toggle').style.position = 'absolute';
el('ap-toggle').style.left = '200px';
el('ap-toggle').style.top = '7px';
flush();
const tr = el('ap-toggle').getBoundingClientRect();
assert(Math.abs(tr.left - (root.getBoundingClientRect().left + 200)) < 0.5 &&
       Math.abs(tr.top - (rootTop + 7)) < 0.5,
       'toggle became absolute: ' + tr.left + ',' + tr.top);

// ...and back into flow.
el('ap-toggle').style.position = 'static';
flush();
assert(top('ap-toggle') > rootTop + 200,
       'toggle back in flow below grown spacer+holder: ' + top('ap-toggle'));

// Badge keeps tracking after the churn above.
el('ap-spacer').style.height = '60px';
flush();
assert(Math.abs(top('ap-badge') - (rootTop + 65)) < 0.5,
       'badge follows again: ' + top('ap-badge'));

// ── scope ───────────────────────────────────────────────────────────────────

// A leaf text change deep in the abs-free rows: the pass must not lay out (or
// re-measure) the world. The bound is loose — the point is O(change), not
// O(document): 100 clean rows would push nodesLaidOut past 100 on their own.
perf.reset();
rows.children[50].textContent = 'changed';
flush();
const p = perf.stats();
assert(p.nodesLaidOut < 60,
       'leaf change stays scoped with positioned elements present: ' +
       p.nodesLaidOut + ' nodes laid out');

root.innerHTML = '';

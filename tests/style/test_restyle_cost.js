// What it costs to put already-built elements back into the document: the
// style and layout bill per element, under a real app stylesheet.
//
// The sheet is the helm app kit's (tests/style/kit_css/, a copy of
// helmapps/lib/css): ~1300 lines, a hundred custom properties on :root, a
// `*, *::before, *::after` reset, and the list rules a VirtualList row is
// styled by. The rows are built the way the kit's VirtualList builds a song
// list (a positioned flex row, five columns with inline flex styles, a span
// in each), detached, and appended again; only that re-insertion's style and
// layout pass is measured. A view switch in an app is exactly this.
//
// It reports the median microseconds per element for style (resolveStyles,
// and of that the cascade) and for layout (build + invalidate + layout +
// sync), and guards both against a regression well past what they are now.
//
// BRO_RESTYLE_COST_ITERS overrides the iteration count (profiling runs).

const fs = require('fs');
const path = require('path');

function findKitCss() {
    let dir = process.cwd();
    for (let i = 0; i < 8; i++) {
        const cand = path.join(dir, 'tests', 'style', 'kit_css');
        if (fs.existsSync(cand)) return cand;
        const up = path.dirname(dir);
        if (up === dir) break;
        dir = up;
    }
    return null;
}

const cssDir = findKitCss();
assert(cssDir, 'tests/style/kit_css found from the working directory');
for (const name of ['tokens.css', 'controls.css', 'layout.css', 'overlays.css']) {
    const el = document.createElement('style');
    el.textContent = fs.readFileSync(path.join(cssDir, name), 'utf8');
    document.head.appendChild(el);
}

function h(tag, cls, text) {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text != null) el.textContent = text;
    return el;
}

const COLUMNS = [
    { width: 48, right: true, cls: 'num', text: (i) => String(i % 12 + 1) },
    { flex: 2, cls: 'ellipsis mu-title', text: (i) => `Song number ${i}` },
    { flex: 1, cls: 'ellipsis', text: (i) => `Artist ${i % 17}` },
    { flex: 1, cls: 'ellipsis', text: (i) => `Album ${i % 9}` },
    { width: 72, right: true, cls: 'num', text: (i) => `${3 + i % 3}:${String(10 + i % 50)}` },
];

function makeRow(i) {
    const row = h('div', 'vlist-row');
    row.setAttribute('role', 'option');
    row.dataset.index = String(i);
    row.style.top = `${i * 34}px`;
    row.style.height = '34px';
    for (const c of COLUMNS) {
        const col = h('div', c.right ? 'col right' : 'col');
        if (c.width) { col.style.width = `${c.width}px`; col.style.flex = 'none'; }
        else { col.style.width = ''; col.style.flex = `${c.flex} 1 0px`; }
        col.appendChild(h('span', c.cls, c.text(i)));
        row.appendChild(col);
    }
    return row;
}

const wrap = h('div', 'vlist-wrap');
wrap.style.cssText = 'position:absolute; left:0; top:0; width:1000px; height:800px; display:flex; flex-direction:column';
const scroller = h('div', 'vlist');
const spacer = h('div', 'vlist-spacer');
scroller.appendChild(spacer);
wrap.appendChild(scroller);
document.body.appendChild(wrap);

const median = (xs) => { const s = xs.slice().sort((a, b) => a - b); return s[s.length >> 1]; };

// `fresh`: every iteration builds new rows (a list filling rows it never had)
// instead of putting the same ones back.
function measure(nRows, iters, fresh = false) {
    let rows = [];
    for (let i = 0; i < nRows; i++) rows.push(makeRow(i));
    spacer.style.height = `${nRows * 34}px`;
    spacer.append(...rows);
    flush();
    const style = [], cascade = [], layout = [], wall = [];
    let last = null;
    for (let k = 0; k < iters; k++) {
        spacer.replaceChildren();
        flush();
        if (fresh) {
            rows = [];
            for (let i = 0; i < nRows; i++) rows.push(makeRow(i + k));
        }
        perf.reset();
        const t0 = perf.now();
        spacer.append(...rows);
        flush();
        const ms = perf.now() - t0;
        const p = perf.stats();
        const n = p.elementsStyled;
        style.push(p.styleMs * 1000 / n);
        cascade.push(p.cascadeMs * 1000 / n);
        layout.push((p.buildMs + p.invalidateMs + p.layoutMs + p.syncMs) * 1000 / n);
        wall.push(ms);
        last = p;
    }
    spacer.replaceChildren();
    flush();
    const r = {
        elements: last.elementsStyled, style: median(style), cascade: median(cascade),
        layout: median(layout), wallMs: median(wall), stats: last,
    };
    console.log(`${nRows} ${fresh ? 'new' : 're-inserted'} rows, ${r.elements} elements: style ${r.style.toFixed(2)} us/el ` +
        `(cascade ${r.cascade.toFixed(2)}), layout ${r.layout.toFixed(2)} us/el, ` +
        `frame ${r.wallMs.toFixed(2)} ms; kept ${last.stylesKept}, shared ${last.stylesShared}, ` +
        `resolved ${last.stylesResolved}, pseudoResolves ${last.pseudoResolves}, ` +
        `layoutSubtreesKept ${last.layoutSubtreesKept}, nodesLaidOut ${last.nodesLaidOut}, nodeVisits ${last.nodeVisits}, measureCalls ${last.measureCalls}, ` +
        `styleLookups ${last.styleLookups}`);
    return r;
}

const iters = Number((process.env && process.env.BRO_RESTYLE_COST_ITERS) || 15);
// BRO_RESTYLE_COST_ONLY=reinsert|fresh runs one case (a profile of one).
const only = (process.env && process.env.BRO_RESTYLE_COST_ONLY) || '';
const fresh = only !== 'reinsert' ? measure(25, iters, true) : null;
if (only === 'fresh') { console.log('fresh only'); }
const small = only !== 'fresh' ? measure(25, iters) : null;
const big = only !== 'fresh' ? measure(100, Math.max(5, iters >> 1)) : null;
if (only) skipTest(`BRO_RESTYLE_COST_ONLY=${only}: a profiling run`);

// Every element put back is styled, and laid out.
assert(small.elements >= 25 * 11, `every re-inserted element is styled (${small.elements})`);
assert(big.elements >= 100 * 11, `every re-inserted element is styled (${big.elements})`);

// The guard. Before style sharing and layout retention a re-inserted element
// cost ~11-12 us of style and ~5-6 us of layout here; now it is ~1.8 and
// ~0.3 (Windows, Release). The limits leave room for a loaded machine running
// the suite in parallel and still fail if either cache stops working.
for (const r of [small, big]) {
    const n = r.elements;
    assert(r.style < 6, `re-insert style ${r.style.toFixed(2)} us/el, limit 6`);
    assert(r.layout < 3, `re-insert layout ${r.layout.toFixed(2)} us/el, limit 3`);
    // And the reasons, which do not depend on the machine: every element kept
    // its style, every row its layout subtree, and next to nothing was laid out.
    assert(r.stats.stylesKept >= n - 2, `styles kept on re-insert (${r.stats.stylesKept} of ${n})`);
    assert(r.stats.layoutSubtreesKept >= Math.floor(n / 11), `row layouts kept (${r.stats.layoutSubtreesKept})`);
    assert(r.stats.nodesLaidOut < 20, `nodes laid out on re-insert (${r.stats.nodesLaidOut})`);
}

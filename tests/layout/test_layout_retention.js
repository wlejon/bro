// Layout retention (LayoutNodeAdapter::markDirtyFromElements): when a parent's
// layout children are rebuilt, a child still there keeps its layout subtree, and
// one taken out parks its subtree on its element so putting it back hands the
// subtree — and its cached geometry — back. That geometry is only right if every
// way it could have gone stale while the element was out of the tree makes it be
// laid out again. Each case changes one such input while an element is detached
// (or moving) and checks the box follows it.

const root = document.getElementById('root');

const sheet = document.createElement('style');
sheet.textContent = `
  #lr { font: 16px Arial; }
  .lr-box { width: 400px; }
  .lr-narrow { width: 200px; }
  .lr-row { display: flex; height: 20px; }
  .lr-col { flex: 0 0 auto; }
  .lr-fill { width: 100%; height: 10px; }
  .lr-vw { width: 25vw; height: 10px; }
  .lr-rem { width: 10rem; height: 10px; }
  .lr-tall { height: 50px; }
  .lr-abs-host { position: relative; height: 40px; }
  .lr-abs { position: absolute; right: 0; top: 0; width: 10px; height: 10px; }
`;
document.head.appendChild(sheet);

const box = document.createElement('div');
box.id = 'lr';
root.appendChild(box);

const eq = (got, want, msg) => assert(got === want, `${msg}: got ${got}, want ${want}`);
const near = (got, want, msg) => assert(Math.abs(got - want) < 0.5, `${msg}: got ${got}, want ${want}`);
const el = (cls, text) => {
    const e = document.createElement('div');
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
};
const rect = (e) => e.getBoundingClientRect();

// ---- the counter: re-inserted rows keep their layout ------------------------------------
{
    const list = el('lr-box');
    box.appendChild(list);
    const rows = [];
    for (let i = 0; i < 20; i++) {
        const r = el('lr-row');
        for (let j = 0; j < 4; j++) r.appendChild(el('lr-col', `cell ${i}.${j}`));
        rows.push(r);
        list.appendChild(r);
    }
    flush();
    const before = rows.map(r => rect(r.children[2]).left);
    for (const r of rows) r.remove();
    flush();
    perf.reset();
    for (const r of rows) list.appendChild(r);
    flush();
    const p = perf.stats();
    assert(p.layoutSubtreesKept >= 20, `put back unchanged, the rows keep their layout (${p.layoutSubtreesKept} kept)`);
    assert(p.nodesLaidOut < 20, `and are not laid out again (${p.nodesLaidOut} laid out)`);
    const after = rows.map(r => rect(r.children[2]).left);
    eq(JSON.stringify(after), JSON.stringify(before), 'with the geometry they had');
    near(rect(rows[5]).top - rect(list).top, 100, 'the sixth row sits at 100px');

    // Appending one row keeps the other twenty.
    perf.reset();
    const extra = el('lr-row');
    extra.appendChild(el('lr-col', 'extra'));
    list.appendChild(extra);
    flush();
    assert(perf.stats().layoutSubtreesKept >= 20, `an append keeps the siblings (${perf.stats().layoutSubtreesKept})`);
    near(rect(extra).top - rect(list).top, 400, 'the new row goes after them');
    list.remove();
    flush();
}

// ---- content changed while detached ------------------------------------------------------
{
    const list = el('lr-box');
    box.appendChild(list);
    const r = el('lr-row');
    const a = el('lr-col', 'ab');
    const b = el('lr-col', 'cd');
    r.append(a, b);
    list.appendChild(r);
    flush();
    const w0 = rect(a).width;
    const b0 = rect(b).left;

    r.remove(); flush();
    a.textContent = 'a much longer label';
    list.appendChild(r);
    flush();
    assert(rect(a).width > w0 + 20, `text grown while detached: the column widens (${w0} -> ${rect(a).width})`);
    assert(rect(b).left > b0 + 20, `and its sibling moves over (${b0} -> ${rect(b).left})`);

    r.remove(); flush();
    a.className = 'lr-col lr-tall';
    list.appendChild(r);
    flush();
    near(rect(a).height, 50, 'a class changed while detached: re-laid out');

    r.remove(); flush();
    const c = el('lr-col', 'ef');
    r.appendChild(c);
    list.appendChild(r);
    flush();
    near(rect(c).left, rect(b).right, 'a child appended while detached is placed after the others');

    r.remove(); flush();
    b.remove();
    list.appendChild(r);
    flush();
    near(rect(c).left, rect(a).right, 'a child removed while detached: the next one moves up');

    // A grandchild's text, two levels inside the parked subtree.
    const wrap = el('', '');
    const inner = el('lr-row');
    const leaf = el('lr-col', 'x');
    inner.appendChild(leaf);
    wrap.appendChild(inner);
    list.appendChild(wrap);
    flush();
    const lw = rect(leaf).width;
    wrap.remove(); flush();
    leaf.textContent = 'xxxxxxxxxxxxxxxxxxxx';
    list.appendChild(wrap);
    flush();
    assert(rect(leaf).width > lw + 50, `a grandchild's text changed while detached (${lw} -> ${rect(leaf).width})`);
    list.remove();
    flush();
}

// ---- a different parent -----------------------------------------------------------------
{
    const wide = el('lr-box');
    const narrow = el('lr-narrow');
    box.append(wide, narrow);
    const f = el('lr-fill');
    wide.appendChild(f);
    flush();
    near(rect(f).width, 400, '100% of the wide parent');

    // Detached, then put under the narrow one.
    f.remove(); flush();
    narrow.appendChild(f);
    flush();
    near(rect(f).width, 200, 'parked, then put under the narrow parent: 100% of it');

    // Moved in one step, both ways round (which parent the rebuild visits first
    // decides whether the subtree is parked and taken, or dropped and rebuilt).
    wide.appendChild(f);
    flush();
    near(rect(f).width, 400, 'moved to the wide parent in one frame');
    narrow.appendChild(f);
    flush();
    near(rect(f).width, 200, 'and back');

    // The same parent, laid out under another width in between.
    f.remove(); flush();
    narrow.className = 'lr-box';
    flush();
    narrow.appendChild(f);
    flush();
    near(rect(f).width, 400, 'its parent widened while it was out');
    wide.remove(); narrow.remove();
    flush();
}

// ---- document-wide inputs: the viewport, the root font size -------------------------------
{
    const holder = el('lr-box');
    box.appendChild(holder);
    const v = el('lr-vw');
    const r = el('lr-rem');
    holder.append(v, r);
    flush();
    const vw0 = innerWidth;
    near(rect(v).width, vw0 / 4, '25vw');
    near(rect(r).width, 160, '10rem at 16px');

    v.remove(); r.remove(); flush();
    resize(1000, 700);
    flush();
    holder.append(v, r);
    flush();
    near(rect(v).width, 250, 'the viewport resized while detached: 25vw follows');

    r.remove(); flush();
    document.documentElement.style.fontSize = '20px';
    flush();
    holder.appendChild(r);
    flush();
    near(rect(r).width, 200, 'the root font size changed while detached: 10rem follows');
    document.documentElement.style.fontSize = '';
    flush();
    holder.remove();
    flush();
}

// ---- a positioned descendant inside a kept subtree -------------------------------------------
{
    const holder = el('lr-box');
    box.appendChild(holder);
    const host = el('lr-abs-host');
    const abs = el('lr-abs');
    host.appendChild(abs);
    holder.appendChild(host);
    flush();
    near(rect(abs).right, rect(host).right, 'right: 0 against its containing block');
    host.remove(); flush();
    holder.className = 'lr-narrow';
    holder.appendChild(host);
    flush();
    near(rect(abs).right, rect(host).right, 'still against it after the parent narrowed');
    near(rect(host).width, 200, 'which is the narrow width');
    holder.remove();
    flush();
}

// The paint cache fuzz's document and steps, shared by the headless gate
// (headless.js, run by tests/layout/test_paint_cache_fuzz.js) and the
// windowed pass (main.js). Everything random comes from one seeded PRNG, so a
// seed replays the same document and the same steps.
//
// After every step the driver compares the frame the span cache and the
// partial raster produced with the same frame recorded and rastered whole
// (setPaintCache(false)). The rule (diff below): a pixel is apart when a
// channel differs by more than 16, and the frames differ where an apart pixel
// has its whole 3x3 neighbourhood apart. A partial raster draws its damage on
// a scratch surface whose origin is not the layer's, and Skia's antialiasing
// of an edge (CPU and GPU alike) moves with that, so edge pixels can flip: a
// line a pixel or two wide along an edge (a rotated one's is a diagonal
// band), reported as edgePixels, never a solid area. A span reused when it
// had changed, or a damage that missed something, leaves a solid area of
// stale pixels, which fails.

(function () {
    function mulberry32(a) {
        return function () {
            a |= 0;
            a = (a + 0x6d2b79f5) | 0;
            let t = Math.imul(a ^ (a >>> 15), 1 | a);
            t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    const WORDS = ['paint', 'span', 'cache', 'bro', 'fuzz', 'Wq', 'lorem ipsum', '12345', 'hover me', 'xyz'];
    const COLORS = ['#e33', '#3a3', '#33e', '#ee3', '#3ee', '#e3e', '#888', '#fff', '#222', '#f80'];

    function create(seed) {
        const r = mulberry32(seed);
        const pick = (arr) => arr[Math.floor(r() * arr.length)];
        const int = (lo, hi) => lo + Math.floor(r() * (hi - lo + 1));
        const stage = document.getElementById('stage');

        function makeBox(depth) {
            const k = r();
            if (depth > 0 && k < 0.05) {
                const ol = document.createElement('ol');
                ol.className = 'list';
                for (let i = int(1, 4); i > 0; --i) {
                    const li = document.createElement('li');
                    // An item with an element in it is a span of its own:
                    // the one whose marker reads its parent.
                    if (r() < 0.5) {
                        const b = document.createElement('b');
                        b.textContent = pick(WORDS);
                        li.appendChild(b);
                    } else {
                        li.textContent = pick(WORDS);
                    }
                    ol.appendChild(li);
                }
                return ol;
            }
            if (depth > 0 && k < 0.18) {
                const s = document.createElement('span');
                s.textContent = pick(WORDS) + ' ';
                if (r() < 0.4) s.style.color = pick(COLORS);
                return s;
            }
            if (depth > 0 && k < 0.19) {
                const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
                svg.setAttribute('width', '16');
                svg.setAttribute('height', '16');
                svg.innerHTML = '<circle cx="8" cy="8" r="6" fill="' + pick(COLORS) + '"/>';
                return svg;
            }
            if (depth > 0 && k < 0.20) {
                const inp = document.createElement('input');
                inp.value = pick(WORDS);
                inp.style.width = int(30, 90) + 'px';
                return inp;
            }
            if (depth > 0 && k < 0.21) {
                const cb = document.createElement('input');
                cb.type = 'checkbox';
                cb.checked = r() < 0.5;
                return cb;
            }
            if (depth > 0 && k < 0.22) {
                const sel = document.createElement('select');
                for (let i = int(2, 4); i > 0; --i) {
                    const o = document.createElement('option');
                    o.textContent = pick(WORDS);
                    sel.appendChild(o);
                }
                return sel;
            }
            const el = document.createElement('div');
            let cls = 'b c' + int(0, 4);
            if (r() < 0.3) cls += ' hov';
            if (r() < 0.15) cls += ' tr';
            if (r() < 0.06) cls += ' anim';
            if (r() < 0.06) cls += ' pulse';
            el.className = cls;
            if (r() < 0.3) el.style.width = int(20, 220) + 'px';
            if (r() < 0.15) el.style.display = 'inline-block';
            if (r() < 0.15) { el.style.position = 'relative'; el.style.left = int(-10, 20) + 'px'; el.style.top = int(-10, 20) + 'px'; }
            if (r() < 0.06) { el.style.position = 'absolute'; el.style.left = int(0, 280) + 'px'; el.style.top = int(0, 200) + 'px'; }
            if (r() < 0.1) el.style.zIndex = String(int(-1, 3));
            if (r() < 0.08) el.style.transform = 'rotate(' + int(-20, 20) + 'deg)';
            if (r() < 0.08) el.style.opacity = String(pick([0.4, 0.7, 0.9]));
            if (depth < 3 && r() < 0.12) el.classList.add('scroller');
            if (r() < 0.5) el.appendChild(document.createTextNode(pick(WORDS)));
            if (depth < 4) {
                const n = depth === 0 ? int(2, 4) : int(0, 3);
                for (let i = 0; i < n; ++i) el.appendChild(makeBox(depth + 1));
            }
            return el;
        }

        let theField = null;  // build()'s text field, worked by the 'field' step

        function build() {
            for (let i = int(5, 8); i > 0; --i) stage.appendChild(makeBox(0));
            // Always a numbered list whose items are spans of their own (their
            // markers read their parent), and a text field to focus and select.
            const ol = document.createElement('ol');
            ol.className = 'list';
            for (let i = 0; i < 4; ++i) {
                const li = document.createElement('li');
                const b = document.createElement('b');
                b.textContent = pick(WORDS);
                li.appendChild(b);
                ol.appendChild(li);
            }
            stage.appendChild(ol);
            // In a box of its own: a span the animations elsewhere on the
            // stage do not record again every frame, so what the field paints
            // is only ever as fresh as its own stamps make it.
            const field = document.createElement('div');
            field.className = 'b c3';
            const inp = document.createElement('input');
            inp.value = 'paint cache';
            inp.style.width = '120px';
            field.appendChild(inp);
            stage.appendChild(field);
            theField = inp;
            // A scroller of plain boxes, at a fractional place: one composited
            // as a scroll layer (layout/scroll_layer.h) until the steps give
            // it something that keeps it in place, as the random ones rarely
            // are.
            const sc = document.createElement('div');
            sc.className = 'b c' + int(0, 4) + ' scroller';
            sc.style.marginLeft = (int(0, 20) + 0.5) + 'px';
            sc.style.width = int(90, 220) + 'px';
            for (let i = int(6, 10); i > 0; --i) {
                const row = document.createElement('div');
                row.className = 'b c' + int(0, 4) + (r() < 0.4 ? ' hov' : '');
                row.textContent = pick(WORDS) + ' ' + pick(WORDS);
                if (r() < 0.3) {
                    const inner = document.createElement('div');
                    inner.className = 'b c' + int(0, 4);
                    inner.textContent = pick(WORDS);
                    row.appendChild(inner);
                }
                sc.appendChild(row);
            }
            stage.appendChild(sc);
        }

        // A random element of the stage (not the stage), optionally filtered.
        function anyEl(filter) {
            const all = Array.from(stage.querySelectorAll('*')).filter((e) => !filter || filter(e));
            return all.length ? all[Math.floor(r() * all.length)] : null;
        }
        const isBox = (e) => e.tagName === 'DIV';

        // Each returns a description, and may ask for time to pass (ms) so
        // transitions and animations are caught mid-flight.
        const steps = [
            [8, 'class', () => {
                const e = anyEl(isBox); if (!e) return null;
                const c = 'c' + int(0, 4);
                for (let i = 0; i < 5; ++i) e.classList.remove('c' + i);
                e.classList.add(c);
                return { what: 'class ' + c };
            }],
            [5, 'text', () => {
                const e = anyEl((x) => x.tagName === 'SPAN' || x.tagName === 'LI'); if (!e) return null;
                e.textContent = pick(WORDS);
                return { what: 'text' };
            }],
            [5, 'insert', () => {
                const p = anyEl(isBox) || stage;
                const kids = p.childNodes;
                p.insertBefore(makeBox(1), kids[int(0, kids.length)] || null);
                return { what: 'insert' };
            }],
            [4, 'remove', () => {
                const e = anyEl(); if (!e) return null;
                e.remove();
                return { what: 'remove ' + e.tagName };
            }],
            [6, 'size', () => {
                const e = anyEl(isBox); if (!e) return null;
                const prop = pick(['width', 'height', 'padding', 'marginTop', 'borderWidth']);
                e.style[prop] = int(0, prop === 'width' ? 240 : 40) + 'px';
                return { what: 'size ' + prop };
            }],
            [4, 'scroll', () => {
                const e = anyEl((x) => x.classList.contains('scroller'));
                if (e) { e.scrollTop = int(0, 120); return { what: 'scroll box' }; }
                document.scrollingElement.scrollTop = int(0, 200);
                return { what: 'scroll document' };
            }],
            [3, 'docscroll', () => {
                document.scrollingElement.scrollTop = int(0, 300);
                return { what: 'scroll document' };
            }],
            [4, 'transform', () => {
                const e = anyEl(isBox); if (!e) return null;
                e.style.transform = pick(['none', 'rotate(10deg)', 'scale(1.3)', 'translate(15px, 5px)', 'skewX(12deg)']);
                return { what: 'transform' };
            }],
            [4, 'opacity', () => {
                const e = anyEl(isBox); if (!e) return null;
                e.style.opacity = String(pick([1, 0.5, 0.2, 0]));
                return { what: 'opacity' };
            }],
            [4, 'clip', () => {
                const e = anyEl(isBox); if (!e) return null;
                if (r() < 0.5) e.style.overflow = pick(['hidden', 'visible', 'auto']);
                else e.style.clipPath = pick(['none', 'polygon(0 0, 100% 0, 50% 100%)', 'polygon(10% 10%, 90% 10%, 90% 90%)']);
                return { what: 'clip' };
            }],
            [3, 'position', () => {
                const e = anyEl(isBox); if (!e) return null;
                e.style.position = pick(['static', 'relative', 'absolute']);
                e.style.zIndex = pick(['auto', '-1', '0', '2']);
                e.style.left = int(-10, 200) + 'px';
                e.style.top = int(-10, 150) + 'px';
                return { what: 'position' };
            }],
            [3, 'display', () => {
                const e = anyEl(); if (!e) return null;
                e.style.display = e.style.display === 'none' ? '' : 'none';
                return { what: 'display' };
            }],
            [7, 'hover', () => {
                return { what: 'hover', hover: [int(0, 359), int(0, 259)] };
            }],
            [5, 'transition', () => {
                const e = anyEl((x) => x.classList.contains('tr'));
                if (e) e.classList.toggle('lit');
                return { what: 'transition', advance: int(30, 300) };
            }],
            [5, 'time', () => {
                return { what: 'time', advance: int(16, 500) };
            }],
            [3, 'font', () => {
                const e = anyEl(); if (!e) return null;
                e.style.fontFamily = pick(['FuzzFace', 'sans-serif', 'serif', 'monospace']);
                return { what: 'font' };
            }],
            [2, 'dpr', () => {
                return { what: 'dpr', dpr: pick([1, 2, 1.5]) };
            }],
            [2, 'ordinal', () => {
                const e = anyEl((x) => x.tagName === 'OL'); if (!e) return null;
                e.setAttribute('start', String(int(1, 9)));
                return { what: 'ordinal' };
            }],
            [5, 'listItem', () => {
                // A numbered item's marker counts its siblings: one put in,
                // taken out, or renumbered moves every number after it.
                const ol = anyEl((x) => x.tagName === 'OL'); if (!ol) return null;
                const items = ol.children;
                const k = r();
                if (k < 0.4 || !items.length) {
                    const li = document.createElement('li');
                    li.textContent = pick(WORDS);
                    ol.insertBefore(li, items[int(0, items.length)] || null);
                    return { what: 'list item in' };
                }
                if (k < 0.7) { items[int(0, items.length - 1)].remove(); return { what: 'list item out' }; }
                items[int(0, items.length - 1)].setAttribute('value', String(int(1, 20)));
                return { what: 'list item value' };
            }],
            [4, 'svg', () => {
                const c = anyEl((x) => x.tagName.toLowerCase() === 'circle'); if (!c) return null;
                const attr = pick(['fill', 'r', 'cx', 'stroke']);
                c.setAttribute(attr, attr === 'fill' || attr === 'stroke' ? pick(COLORS) : String(int(2, 8)));
                return { what: 'svg ' + attr };
            }],
            [5, 'field', () => {
                // The text field build() made: focused, then its selection
                // moved and text typed into it, which no restyle follows.
                const e = theField;
                if (!e || !e.isConnected) return null;
                if (document.activeElement !== e) { e.focus(); return { what: 'field focus' }; }
                const k = r();
                if (k < 0.45) {
                    const n = e.value.length;
                    const a = int(0, n), b = int(0, n);
                    e.setSelectionRange(Math.min(a, b), Math.max(a, b));
                    return { what: 'field selection' };
                }
                if (k < 0.9 && typeof textInput === 'function') {
                    textInput(pick(['a', 'Wq', ' ', 'xyz']));
                    return { what: 'field typed' };
                }
                e.blur();
                return { what: 'field blur' };
            }],
            [5, 'control', () => {
                // Half the time the field that has focus, if one does: what
                // it paints then (caret, selection, typed text) moves with no
                // restyle.
                const active = document.activeElement;
                const focused = active && active.tagName === 'INPUT' && active.type !== 'checkbox' && stage.contains(active);
                const e = focused && r() < 0.5 ? active
                    : anyEl((x) => x.tagName === 'INPUT' || x.tagName === 'SELECT');
                if (!e) return null;
                const k = r();
                if (document.activeElement === e && e.type !== 'checkbox' && k < 0.3 && typeof textInput === 'function') {
                    textInput(pick(['a', 'Wq', ' ', 'xyz']));
                    return { what: 'input typed' };
                }
                if (e.tagName === 'SELECT') { e.selectedIndex = int(0, e.options.length - 1); return { what: 'select index' }; }
                if (e.type === 'checkbox') { e.checked = !e.checked; return { what: 'checkbox' }; }
                if (document.activeElement === e && k < 0.6) {
                    // Focused, it paints its selection: which moves with no
                    // restyle at all.
                    const n = e.value.length;
                    const a = int(0, n), b = int(0, n);
                    e.setSelectionRange(Math.min(a, b), Math.max(a, b));
                    return { what: 'input selection' };
                }
                if (k < 0.4) { e.value = pick(WORDS); return { what: 'input value' }; }
                if (k < 0.55) { e.disabled = !e.disabled; return { what: 'input disabled' }; }
                if (document.activeElement === e) e.blur(); else e.focus();
                return { what: 'input focus' };
            }],
            [2, 'animate', () => {
                const e = anyEl(isBox); if (!e) return null;
                e.classList.toggle(pick(['anim', 'pulse']));
                return { what: 'animation', advance: int(16, 200) };
            }],
        ];
        const total = steps.reduce((s, x) => s + x[0], 0);

        function step() {
            for (;;) {
                let k = r() * total;
                for (const [w, name, run] of steps) {
                    k -= w;
                    if (k < 0) {
                        const res = run();
                        if (res) return Object.assign({ name }, res);
                        break;
                    }
                }
            }
        }

        return { build, step };
    }

    // Whether two RGBA frames are the same, bit for bit.
    function same(a, b) {
        if (!a || !b || a.width !== b.width || a.height !== b.height) return false;
        const wa = new Uint32Array(a.data.buffer, a.data.byteOffset, a.data.length >> 2);
        const wb = new Uint32Array(b.data.buffer, b.data.byteOffset, b.data.length >> 2);
        for (let p = 0; p < wa.length; ++p) if (wa[p] !== wb[p]) return false;
        return true;
    }

    // A frame that redrew only its damage (cached) against one drawn whole
    // (full), by the rule in this file's header: a pixel is apart when a
    // channel differs by more than EDGE, and the frames differ (`count`) at
    // every apart pixel whose whole 3x3 neighbourhood is apart; `edge` counts
    // the rest. PAINT_FUZZ_EDGE tunes EDGE.
    const EDGE = Number((typeof process !== 'undefined' && process.env && process.env.PAINT_FUZZ_EDGE) || 16);
    function diff(a, b) {
        if (!a || !b) return { count: -1, edge: 0, first: null };
        if (a.width !== b.width || a.height !== b.height)
            return { count: -2, edge: 0, first: [a.width, a.height, b.width, b.height] };
        const da = a.data, db = b.data;
        const W = a.width, H = a.height;
        // A pixel a word: a quarter of the compares.
        const wa = new Uint32Array(da.buffer, da.byteOffset, da.length >> 2);
        const wb = new Uint32Array(db.buffer, db.byteOffset, db.length >> 2);
        const apart = new Uint8Array(W * H);
        const marked = [];
        for (let p = 0; p < wa.length; ++p) {
            if (wa[p] === wb[p]) continue;
            const i = p * 4;
            if (Math.abs(da[i] - db[i]) > EDGE || Math.abs(da[i + 1] - db[i + 1]) > EDGE ||
                Math.abs(da[i + 2] - db[i + 2]) > EDGE || Math.abs(da[i + 3] - db[i + 3]) > EDGE) {
                apart[p] = 1;
                marked.push(p);
            }
        }
        let count = 0, edge = 0, firstAt = -1;
        let ex0 = W, ey0 = H, ex1 = -1, ey1 = -1;  // the edge pixels' bounds
        for (const p of marked) {
            const x = p % W, y = (p - x) / W;
            let near = 0;
            for (let dy = -1; dy <= 1; ++dy)
                for (let dx = -1; dx <= 1; ++dx) {
                    const xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < W && yy < H && apart[yy * W + xx]) ++near;
                }
            if (near === 9) {
                if (firstAt < 0) firstAt = p;
                ++count;
            } else {
                ++edge;
                ex0 = Math.min(ex0, x); ey0 = Math.min(ey0, y);
                ex1 = Math.max(ex1, x); ey1 = Math.max(ey1, y);
            }
        }
        let first = null;
        if (firstAt >= 0) {
            const i = firstAt * 4;
            first = [firstAt % W, Math.floor(firstAt / W),
                     [da[i], da[i + 1], da[i + 2], da[i + 3]].join(','),
                     [db[i], db[i + 1], db[i + 2], db[i + 3]].join(',')];
        }
        return { count, edge, first, edgeBounds: edge ? [ex0, ey0, ex1, ey1] : null };
    }

    globalThis.PaintFuzz = { create, diff, same };
})();

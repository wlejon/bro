// The paint cache fuzz's document and steps, shared by the headless gate
// (headless.js, run by tests/layout/test_paint_cache_fuzz.js) and the
// windowed pass (main.js). Everything random comes from one seeded PRNG, so a
// seed replays the same document and the same steps.
//
// After every step the driver compares the frame the span cache produced with
// a full re-record of it (setPaintCache(false)): any pixel apart is a span
// reused when what it painted had changed.

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
            if (depth > 0 && k < 0.02) {
                const ol = document.createElement('ol');
                ol.className = 'list';
                for (let i = int(1, 3); i > 0; --i) {
                    const li = document.createElement('li');
                    li.textContent = pick(WORDS);
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

        function build() {
            for (let i = int(5, 8); i > 0; --i) stage.appendChild(makeBox(0));
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
                window.scrollTo(0, int(0, 200));
                return { what: 'scroll document' };
            }],
            [3, 'docscroll', () => {
                window.scrollTo(0, int(0, 300));
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

    // Pixels apart between two RGBA frames: { count, first: [x, y, a, b] }.
    function diff(a, b) {
        if (!a || !b) return { count: -1, first: null };
        if (a.width !== b.width || a.height !== b.height)
            return { count: -2, first: [a.width, a.height, b.width, b.height] };
        const da = a.data, db = b.data;
        // A pixel a word: a quarter of the compares.
        const wa = new Uint32Array(da.buffer, da.byteOffset, da.length >> 2);
        const wb = new Uint32Array(db.buffer, db.byteOffset, db.length >> 2);
        let count = 0, firstAt = -1;
        const n = wa.length;
        for (let p = 0; p < n; ++p) {
            if (wa[p] !== wb[p]) {
                if (firstAt < 0) firstAt = p;
                ++count;
            }
        }
        let first = null;
        if (firstAt >= 0) {
            const i = firstAt * 4;
            first = [firstAt % a.width, Math.floor(firstAt / a.width),
                     [da[i], da[i + 1], da[i + 2], da[i + 3]].join(','),
                     [db[i], db[i + 1], db[i + 2], db[i + 3]].join(',')];
        }
        return { count, first };
    }

    globalThis.PaintFuzz = { create, diff };
})();

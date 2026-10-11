// The style and layout walks descend only where something is marked
// (Element::notePathDirty), and a running animation re-resolves its element
// every frame without anyone marking it. So the style pass puts the path to
// each running animation back (WebAnimationManager::activeThisTick): an
// animated element deep in a subtree nothing else touches keeps animating,
// whether the animation is a script one, a CSS @keyframes one or a CSS
// transition, and its geometry follows.

const root = document.getElementById('root');
const style = document.createElement('style');
style.textContent = `
  @keyframes ra-spin { from { opacity: 0; } to { opacity: 1; } }
  .ra-css { animation: ra-spin 1000ms linear infinite; }
  .ra-tr { width: 10px; transition: width 1000ms linear; }
  .ra-tr.wide { width: 210px; }
`;
document.head.appendChild(style);

// A deep, quiet subtree with one animated leaf in it.
function deep(leaf) {
    let n = leaf;
    for (let i = 0; i < 12; i++) {
        const w = document.createElement('div');
        for (let k = 0; k < 4; k++) w.append(document.createElement('span'));
        w.append(n);
        n = w;
    }
    return n;
}
const frame = () => advanceTime(16);
const op = (el) => parseFloat(getComputedStyle(el).opacity);
const settleTo = (ms) => { for (let t = 0; t < ms; t += 16) frame(); };

// ---- a script animation -------------------------------------------------------------
{
    const leaf = document.createElement('div');
    leaf.textContent = 'a';
    root.append(deep(leaf));
    frame();
    leaf.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 1000, iterations: Infinity });
    frame();
    settleTo(200);
    const a = op(leaf);
    settleTo(300);
    const b = op(leaf);
    assert(a > 0.05 && a < 0.5 && b > a + 0.15, `element.animate deep in a quiet subtree keeps moving: ${a} -> ${b}`);
}

// ---- a CSS @keyframes animation ---------------------------------------------------------
{
    const leaf = document.createElement('div');
    leaf.textContent = 'c';
    root.append(deep(leaf));
    frame();
    leaf.className = 'ra-css';
    frame();
    settleTo(200);
    const a = op(leaf);
    settleTo(300);
    const b = op(leaf);
    assert(b > a + 0.15, `a CSS animation deep in a quiet subtree keeps moving: ${a} -> ${b}`);
}

// ---- a CSS transition, and the geometry it moves ------------------------------------------
{
    const leaf = document.createElement('div');
    leaf.className = 'ra-tr';
    leaf.textContent = 't';
    root.append(deep(leaf));
    frame();
    leaf.classList.add('wide');
    frame();
    settleTo(250);
    const w1 = leaf.getBoundingClientRect().width;
    settleTo(300);
    const w2 = leaf.getBoundingClientRect().width;
    assert(w1 > 20 && w2 > w1 + 30, `a transition deep in a quiet subtree keeps moving its box: ${w1} -> ${w2}`);
    settleTo(700);
    const w3 = leaf.getBoundingClientRect().width;
    assert(Math.abs(w3 - 210) < 1, `and lands on its end value: ${w3}`);
}
root.replaceChildren();
style.remove();
frame();

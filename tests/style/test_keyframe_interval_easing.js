// A CSS animation's timing function eases each keyframe INTERVAL, not the
// whole iteration, and a keyframe's own animation-timing-function replaces it
// for the interval that keyframe starts. The whole iteration used to be
// eased once, so a 0% / 50% / 100% animation ran a single curve end to end
// and reached the 50% keyframe late (and a WAAPI animation with the same
// per-keyframe easing drifted away from it).

const style = document.createElement('style');
style.textContent = `
  @keyframes slide { 0% { transform: translateX(0px) } 50% { transform: translateX(100px) } 100% { transform: translateX(200px) } }
  @keyframes slideLin {
    0%   { transform: translateX(0px); animation-timing-function: linear }
    50%  { transform: translateX(100px) }
    100% { transform: translateX(200px) }
  }
  .ei  { animation: slide 1000ms cubic-bezier(0.42, 0, 1, 1) both; }
  .own { animation: slideLin 1000ms cubic-bezier(0.42, 0, 1, 1) both; }
`;
document.head.appendChild(style);

const root = document.getElementById('root');
root.innerHTML = '<div class="ei" id="a" style="width:10px;height:10px"></div>' +
                 '<div class="own" id="b" style="width:10px;height:10px"></div>';
flush();

function x(id) {
    const t = getComputedStyle(document.getElementById(id)).transform;
    let m = /translateX\((-?[\d.e+-]+)px\)/.exec(t);
    if (m) return +m[1];
    m = /matrix\(([^)]*)\)/.exec(t);
    if (m) return +m[1].split(',')[4];
    return NaN;
}

advanceTime(250);
flush();
// Halfway through the first interval: ease-in(0.5) of 0..100px is ~31.5px.
// Easing the whole iteration gave ease-in(0.25) of 0..200px, ~18.7px.
const a1 = x('a');
assert(a1 > 25 && a1 < 38, 'the easing applies per interval, got ' + a1);
// The 0% keyframe says linear: 50px.
const b1 = x('b');
assert(b1 > 44 && b1 < 56, "a keyframe's animation-timing-function eases its interval, got " + b1);
assert(!/animation-timing-function/.test(getComputedStyle(document.getElementById('b')).transform),
    'the keyframe easing is not an animated value');

advanceTime(250);
flush();
// At the 50% keyframe the element is at that keyframe's value (~100px), not
// at ease-in(0.5) of the whole run (~63px).
const a2 = x('a');
assert(a2 > 88 && a2 < 112, 'the 50% keyframe is reached at 50%, got ' + a2);

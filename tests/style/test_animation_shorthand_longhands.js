// The `animation` shorthand expands into its longhands: an easing written as
// cubic-bezier(...) (commas inside!) runs, and getComputedStyle reports the
// longhands for an animation set through the shorthand. The shorthand used
// to be split at spaces by the animation manager, which took the pieces of
// cubic-bezier(0.4, 0, 0.2, 1) for a name and an iteration count.

const style = document.createElement('style');
style.textContent = `
  @keyframes kk { 0% { transform: rotate(0deg) } 100% { transform: rotate(360deg) } }
  .cubic { animation: kk 2s cubic-bezier(0.4, 0, 0.2, 1) infinite; }
  .lin { animation: kk 2s linear infinite; }
  .full { animation: kk 3s steps(4, end) 250ms 2 alternate both paused; }
  .multi { animation: kk 1s ease-in, other 2s; }
`;
document.head.appendChild(style);

const root = document.getElementById('root');
root.innerHTML = '<div class="cubic" id="c" style="width:10px;height:10px"></div>' +
                 '<div class="lin" id="l" style="width:10px;height:10px"></div>' +
                 '<div class="full" id="f"></div><div class="multi" id="m"></div>';
flush();
advanceTime(500);
flush();

const cs = (id) => getComputedStyle(document.getElementById(id));
assert(cs('c').transform !== 'none', 'a cubic-bezier() shorthand animates, got ' + cs('c').transform);
assert(cs('l').transform !== 'none', 'a linear shorthand animates, got ' + cs('l').transform);

let s = cs('c');
assert(s.animationName === 'kk', 'animationName from the shorthand, got ' + s.animationName);
assert(s.animationDuration === '2s', 'animationDuration, got ' + s.animationDuration);
assert(/^cubic-bezier\(0\.4, ?0, ?0\.2, ?1\)$/.test(s.animationTimingFunction),
    'animationTimingFunction, got ' + s.animationTimingFunction);
assert(s.animationIterationCount === 'infinite', 'animationIterationCount, got ' + s.animationIterationCount);

s = cs('f');
assert(s.animationDuration === '3s' && s.animationDelay === '250ms',
    'first time is the duration, second the delay: ' + s.animationDuration + ' / ' + s.animationDelay);
assert(/^steps\(4, ?end\)$/.test(s.animationTimingFunction), 'steps() easing, got ' + s.animationTimingFunction);
assert(s.animationIterationCount === '2', 'count, got ' + s.animationIterationCount);
assert(s.animationDirection === 'alternate', 'direction, got ' + s.animationDirection);
assert(s.animationFillMode === 'both', 'fill mode, got ' + s.animationFillMode);
assert(s.animationPlayState === 'paused', 'play state, got ' + s.animationPlayState);

s = cs('m');
assert(s.animationName === 'kk, other', 'one entry per layer, got ' + s.animationName);
assert(s.animationDuration === '1s, 2s', 'durations per layer, got ' + s.animationDuration);
assert(s.animationTimingFunction === 'ease-in, ease', 'easing per layer, got ' + s.animationTimingFunction);

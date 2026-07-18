// Post-reload assertions for the top-level location.reload() + matchMedia
// test. Driven as:  bro-headless tests/engine/mql_reload_app verify.js
// (deliberately not named test_*.js — the suite runner reaches it through
// test_match_media_teardown.js, which spawns this child process).

assert(document.getElementById('stage').textContent === 'second run',
       'this realm is the app\'s second run');

// The fresh realm's matchMedia works against the live viewport.
const W = window.innerWidth;
assert(W > 500, 'default headless viewport is wider than the resize threshold');
assert(matchMedia(`(width: ${W}px)`).matches === true,
       'fresh realm matchMedia evaluates');

// Change events flow in the fresh realm: resize across a threshold. Delivery
// walks every live MediaQueryList of the realm — a wrapper leaked from the
// pre-reload realm would be walked with a dead context here.
let fires = 0;
const narrow = matchMedia('(max-width: 500px)');
narrow.addEventListener('change', (ev) => { fires++; assert(ev.matches === true, 'new state'); });
resize(400, 300);
assert(fires === 1, 'change fired after reload-swapped realm resize, got ' + fires);
resize(W, 600);

// GC sweep with the run-2 pinned, reference-free lists still registered.
advanceTime(1200);
flush();

console.log('MQL_RELOAD_OK');

// The proxy probe: the four live views that needed a property trap —
// el.style, getComputedStyle(el), el.dataset and localStorage.
//
// What is worth pinning here is not that a property round-trips, but the three
// things the accessor-pair shape could not do at all: a property OUTSIDE the
// curated list, a key invented at runtime, and enumeration.

function say(label, value) { console.log('APP ' + label + '=' + value); }

const el = document.createElement('div');
document.body.appendChild(el);

// ---------------------------------------------------------------------------
// 1. style — the curated list is no longer the boundary
// ---------------------------------------------------------------------------
el.style.color = 'red';
say('style.curated', el.style.color === 'red');

// Off the old ~110-name list: these would have read "" no matter what was
// assigned, because no accessor existed for them.
el.style.gridAutoFlow = 'column';
say('style.uncurated.camel', el.style.gridAutoFlow === 'column');
say('style.uncurated.kebab', el.style['grid-auto-flow'] === 'column');

el.style['scrollbar-color'] = 'auto';
say('style.uncurated.kebabSet', el.style.scrollbarColor === 'auto');

// A custom property is not kebab-cased on the way through: `--fooBar` and
// `--foo-bar` are two different properties on the web.
el.style.setProperty('--brandColor', '#0af');
say('style.custom', el.style['--brandColor'] === '#0af');

say('style.unset', el.style.marginTop === '');
say('style.methodsIntact', typeof el.style.setProperty === 'function');
say('style.cssTextIntact', el.style.cssText.indexOf('color: red') >= 0);

// Enumeration is the set properties, in CSS spelling — not all 363.
const styleKeys = Object.keys(el.style).sort();
say('style.keys', styleKeys.join(','));
say('style.in.set', 'color' in el.style);
say('style.in.unset', 'margin-top' in el.style);

delete el.style.color;
say('style.delete', el.style.color === '');

// A symbol key must not throw: toUtf8 of a symbol is a TypeError by spec, so
// a trap that stringified first would break any library that probes one.
say('style.symbol', el.style[Symbol.toStringTag] === undefined);

// ---------------------------------------------------------------------------
// 2. getComputedStyle — same reach, still read-only
// ---------------------------------------------------------------------------
el.style.display = 'block';
const cs = getComputedStyle(el);
say('computed.curated', cs.display === 'block');
say('computed.uncurated', typeof cs.gridAutoFlow === 'string');
say('computed.getPropertyValue', cs.getPropertyValue('display') === 'block');

cs.display = 'inline';
say('computed.readonly', cs.display === 'block');

// ---------------------------------------------------------------------------
// 3. dataset — the surface that did not exist
// ---------------------------------------------------------------------------
el.setAttribute('data-user-id', '42');
say('dataset.fromAttr', el.dataset.userId === '42');

// The write that made a faked dataset worse than none.
el.dataset.role = 'button';
say('dataset.newKey', el.dataset.role === 'button');
say('dataset.writesThrough', el.getAttribute('data-role') === 'button');

// And it is a view, not a snapshot: an attribute set the other way is visible.
el.setAttribute('data-late', 'yes');
say('dataset.live', el.dataset.late === 'yes');

say('dataset.absent', el.dataset.nothing === undefined);
say('dataset.in', 'role' in el.dataset);
say('dataset.keys', Object.keys(el.dataset).sort().join(','));

delete el.dataset.role;
say('dataset.delete', el.dataset.role === undefined && !el.hasAttribute('data-role'));

// Identity: the same object every read, because a UI caches it.
say('dataset.identity', el.dataset === el.dataset);

// ---------------------------------------------------------------------------
// 4. localStorage — named properties over the same map
// ---------------------------------------------------------------------------
localStorage.clear();
localStorage.setItem('viaMethod', 'a');
say('storage.methodThenProp', localStorage.viaMethod === 'a');

localStorage.viaProp = 'b';
say('storage.propThenMethod', localStorage.getItem('viaProp') === 'b');

say('storage.absent', localStorage.nothing === undefined);
say('storage.keys', Object.keys(localStorage).sort().join(','));
say('storage.methodsIntact', typeof localStorage.setItem === 'function');
say('storage.length', localStorage.length === 2);

delete localStorage.viaProp;
say('storage.delete', localStorage.getItem('viaProp') === null);
localStorage.clear();

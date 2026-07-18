// window.matchMedia + MediaQueryList — matches correctness against the
// headless viewport and the appearance.colorScheme setting, change events on
// resize and scheme flips (delivered after the restyle), listener add/remove,
// onchange, the legacy addListener/removeListener aliases, no-fire when
// matches doesn't flip, and keep-alive of listening lists across GC.
//
// Evaluation is htmlayout's @media evaluator against the document's
// MediaContext, so matchMedia agrees with CSS by construction.

const W = window.innerWidth;
const H = window.innerHeight;
assert(W > 500, 'headless viewport is wide enough for the scoping asserts, got ' + W);

// --- basic shape ------------------------------------------------------------
assert(typeof window.matchMedia === 'function', 'window.matchMedia exists');
assert(matchMedia === window.matchMedia, 'matchMedia is a window/global alias');

const mql = matchMedia(`(min-width: ${W}px)`);
assert(typeof mql.matches === 'boolean', 'matches is a boolean');
assert(mql.media === `(min-width: ${W}px)`, 'media reflects the input, got ' + mql.media);
assert(typeof mql.addEventListener === 'function', 'addEventListener present');
assert(typeof mql.removeEventListener === 'function', 'removeEventListener present');
assert(typeof mql.addListener === 'function', 'legacy addListener present');
assert(typeof mql.removeListener === 'function', 'legacy removeListener present');

// --- width/height correctness against the live viewport ---------------------
assert(matchMedia(`(min-width: ${W}px)`).matches === true, 'min-width == viewport matches');
assert(matchMedia(`(min-width: ${W + 1}px)`).matches === false, 'min-width beyond viewport does not');
assert(matchMedia(`(max-width: ${W}px)`).matches === true, 'max-width == viewport matches');
assert(matchMedia(`(max-width: ${W - 1}px)`).matches === false, 'max-width below viewport does not');
assert(matchMedia(`(width: ${W}px)`).matches === true, 'exact width matches');
assert(matchMedia(`(min-height: ${H}px)`).matches === true, 'min-height == viewport matches');
assert(matchMedia(`(min-width: ${W}px) and (min-height: ${H}px)`).matches === true, 'and-chain matches');
assert(matchMedia(`(min-width: ${W + 1}px), (min-height: ${H}px)`).matches === true,
       'comma list matches when any query matches');

// --- media types + garbage --------------------------------------------------
assert(matchMedia('screen').matches === true, 'screen matches');
assert(matchMedia('all').matches === true, 'all matches');
assert(matchMedia('print').matches === false, 'print does not match');
assert(matchMedia('complete garbage !!!').matches === false, 'garbage query: matches false');
assert(matchMedia('complete garbage !!!').media === 'complete garbage !!!',
       'garbage query: media reflects input');
assert(matchMedia('').media === 'all', 'empty query serializes as all');
assert(matchMedia('').matches === true, 'empty query matches');
assert(matchMedia('   screen   ').media === 'screen', 'media is trimmed');

// --- prefers-color-scheme against the setting -------------------------------
bro.settings.set('appearance.colorScheme', 'light');
flush();
const dark = matchMedia('(prefers-color-scheme: dark)');
const light = matchMedia('(prefers-color-scheme: light)');
assert(dark.matches === false, 'forced light: dark query false');
assert(light.matches === true, 'forced light: light query true');

// --- change events on a scheme flip, after the restyle ----------------------
// The listener must observe computed styles already consistent with the new
// scheme (delivery is gated on the media-triggered restyle having landed).
const style = document.createElement('style');
style.textContent = `
  #mm-box { width: 50px; height: 20px; background-color: rgb(10, 20, 30); }
  @media (prefers-color-scheme: dark) {
    #mm-box { background-color: rgb(200, 100, 50); }
  }
`;
document.head.appendChild(style);
const box = document.createElement('div');
box.id = 'mm-box';
document.body.appendChild(box);
flush();

let events = [];
let onchangeFires = 0;
let legacyFires = 0;
let styleAtEvent = '';
const listener = (ev) => {
  events.push({ matches: ev.matches, media: ev.media, type: ev.type });
  styleAtEvent = getComputedStyle(box).backgroundColor;
};
dark.addEventListener('change', listener);
dark.addEventListener('change', listener); // dupe registers once
dark.onchange = () => { onchangeFires++; };
dark.addListener(() => { legacyFires++; });

bro.settings.set('appearance.colorScheme', 'dark');
flush();
assert(events.length === 1, 'one change event on flip to dark, got ' + events.length);
assert(events[0].matches === true, 'event.matches is the new state');
assert(events[0].media === '(prefers-color-scheme: dark)', 'event.media set, got ' + events[0].media);
assert(events[0].type === 'change', 'event.type is change');
assert(onchangeFires === 1, 'onchange fired once, got ' + onchangeFires);
assert(legacyFires === 1, 'legacy addListener fired once, got ' + legacyFires);
assert(dark.matches === true, 'mql.matches updated');
assert(styleAtEvent.indexOf('200') !== -1,
       'listener observed post-restyle styles (dark bg), got ' + styleAtEvent);

// No-fire when matches doesn't flip: setting the same value again.
bro.settings.set('appearance.colorScheme', 'dark');
flush();
assert(events.length === 1, 'no change event when scheme unchanged, got ' + events.length);

// Flip back: fires with matches=false.
bro.settings.set('appearance.colorScheme', 'light');
flush();
assert(events.length === 2, 'change event on flip back, got ' + events.length);
assert(events[0].matches === true && events[1].matches === false, 'flip sequence recorded');
assert(onchangeFires === 2 && legacyFires === 2, 'all listener kinds fired both flips');

// --- removeEventListener / removeListener / onchange clear -------------------
dark.removeEventListener('change', listener);
dark.onchange = null;
assert(dark.onchange === null, 'onchange reads back null after clear');
bro.settings.set('appearance.colorScheme', 'dark');
flush();
assert(events.length === 2, 'removed listener does not fire');
assert(onchangeFires === 2, 'cleared onchange does not fire');
assert(legacyFires === 3, 'remaining legacy listener still fires, got ' + legacyFires);
bro.settings.set('appearance.colorScheme', 'light');
flush();
assert(legacyFires === 4, 'legacy listener fires on flip back');

// A no-flip query never fires: (min-width: 1px) is true before and after.
let alwaysFires = 0;
const always = matchMedia('(min-width: 1px)');
always.addEventListener('change', () => { alwaysFires++; });

// --- change events on resize -------------------------------------------------
const narrow = matchMedia('(max-width: 500px)');
assert(narrow.matches === false, 'narrow query false at full width');
let resizeEvents = [];
narrow.addEventListener('change', (ev) => { resizeEvents.push(ev.matches); });

resize(400, 300);
assert(resizeEvents.length === 1 && resizeEvents[0] === true,
       'resize below threshold fires change(matches=true), got ' + JSON.stringify(resizeEvents));
assert(narrow.matches === true, 'narrow matches after resize');
assert(matchMedia(`(width: 400px)`).matches === true, 'viewport width tracked live');

resize(W, H);
assert(resizeEvents.length === 2 && resizeEvents[1] === false,
       'resize back fires change(matches=false), got ' + JSON.stringify(resizeEvents));
assert(alwaysFires === 0, 'never-flipping query never fired, got ' + alwaysFires);

// --- keep-alive: a listening MQL with no JS reference survives GC ------------
let anonFires = 0;
(function () {
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => { anonFires++; });
})();
advanceTime(1200); // periodic GC runs every ~1s of virtual time
bro.settings.set('appearance.colorScheme', 'dark');
flush();
assert(anonFires === 1, 'listening MQL survives GC with no app reference, got ' + anonFires);

// Cleanup: revert the persisted appearance override.
bro.settings.reset('appearance');
document.body.removeChild(box);
document.head.removeChild(style);

console.log('PASS matchMedia');

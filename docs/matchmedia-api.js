/**
 * window.matchMedia(), MediaQueryList (CSSOM View subset)
 *
 * Programmatic media-query evaluation that reuses the exact same evaluator
 * and MediaContext that filter the document's @media blocks (htmlayout), so
 * matchMedia and CSS can never disagree. Every realm, the app document, every
 * <iframe> sub-document, every secondary window opened with bro.window.open,
 * and every system panel: has its own matchMedia evaluating against ITS
 * document's context: an iframe's queries see the iframe's content box, not
 * the host viewport.
 *
 * KNOWN GAP: secondary windows get .matches but never 'change' events. The
 * engine's change-delivery pass walks the app context, the iframe realms, and
 * the system panels; secondary-window host realms are not in that list, so a
 * MediaQueryList created inside one evaluates correctly on every read and its
 * listeners simply never fire. Poll .matches there (e.g. from a resize
 * listener) instead of relying on 'change'.
 *
 * FEATURE COVERAGE: exactly the CSS side's:
 *   - width / height, min-/max- prefixed, and range syntax
 *     ("(400px <= width <= 800px)", "(width > 500px)")
 *   - orientation: portrait | landscape
 *   - prefers-color-scheme: light | dark: the appearance.colorScheme
 *     setting (system|light|dark) resolved against the OS theme
 *   - media types: screen (matches), all, print (doesn't), "not", "and",
 *     "or", and comma-separated query lists (any-of)
 *   Unsupported or garbage queries evaluate to matches === false (spec
 *   "not all"); .media always reflects the input string (trimmed; "" → "all").
 *   No "not all" normalization is performed on .media.
 *
 * CHANGE EVENTS
 *   'change' fires when a re-evaluation against the updated MediaContext
 *   flips .matches, on window resize and on color-scheme changes (the
 *   appearance.colorScheme setting or an OS theme flip). Delivery happens on
 *   the main thread AFTER the media-triggered restyle has landed, so
 *   listeners always observe getComputedStyle() results consistent with the
 *   new context. The event is a MediaQueryListEvent-SHAPED plain object, not
 *   a real Event: { type: 'change', matches, media, target, currentTarget }
 *   and nothing else: no preventDefault / stopPropagation / bubbles /
 *   timeStamp. Its `matches` is the cached flip value that triggered the
 *   delivery, so it can differ from a live mql.matches read if the context
 *   changed again inside the handler.
 *
 *   Not delivered in secondary windows: see KNOWN GAP above.
 *
 *   Listener registration is a minimal surface, not full EventTarget:
 *     - addEventListener/removeEventListener with fewer than 2 arguments are
 *       silent no-ops, and the third `options` argument is ignored entirely,
 * no once, capture, passive, or signal.
 *     - only the "change" type is honoured; any other type is dropped.
 *     - a non-function listener is silently ignored (no TypeError).
 *     - assigning a non-function to .onchange silently CLEARS the handler
 *       rather than throwing, so `mql.onchange = someUndefinedVar` quietly
 *       unsubscribes.
 *     - registering the same function twice registers it once (spec).
 *
 * LIFETIME
 *   A MediaQueryList with at least one listener (addEventListener /
 *   addListener / onchange) is kept alive for the life of its realm even if
 *   the app drops every reference to it: browser behavior; the listeners
 *   keep firing. Listener-less lists are garbage-collected normally. Realm
 *   teardown (iframe removal/reload, location.reload()) releases everything.
 *
 * NOTES
 *   - .matches is LIVE: reading it re-evaluates against the document's
 *     current context, so it is already fresh immediately after a headless
 *     resize(w, h), even before the change event has been delivered.
 *   - An <iframe> sub-document's media viewport is its element's content box,
 *     and it TRACKS that box: resizing the <iframe> element (directly, or
 *     because a host window resize reflowed it) re-evaluates the
 *     sub-document's CSS @media rules and fires 'change' on its realm's
 *     MediaQueryLists, plus a 'resize' event with fresh innerWidth /
 *     innerHeight inside the sub-document. A host window resize that leaves
 *     the iframe's box unchanged (a fixed-px iframe) correctly changes
 *     nothing inside it. Color-scheme changes also propagate into iframe
 *     realms.
 *   - Headless: resize(w, h) delivers resize-driven change events
 *     synchronously; scheme flips via bro.settings deliver on the next
 *     flush() / advanceTime().
 */

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

const mql = window.matchMedia('(max-width: 600px)');
mql.matches;   // boolean, live evaluation against the current viewport
mql.media;     // "(max-width: 600px)", the query string as given (trimmed)

// Media features agree with CSS @media by construction:
matchMedia('(min-width: 800px)').matches;
matchMedia('(400px <= width <= 1200px)').matches;    // range syntax
matchMedia('(orientation: landscape)').matches;
matchMedia('(prefers-color-scheme: dark)').matches;  // appearance.colorScheme
matchMedia('screen and (min-width: 500px)').matches;
matchMedia('print').matches;                          // false, bro is a screen
matchMedia('(min-width: 2000px), (orientation: landscape)').matches; // any-of

// Garbage in → matches false, media preserved (spec: "not all"):
matchMedia('complete garbage').matches;               // false

// ---------------------------------------------------------------------------
// Change events
// ---------------------------------------------------------------------------

const dark = matchMedia('(prefers-color-scheme: dark)');

// Standard surface:
function onSchemeChange(ev) {
  console.log('dark mode:', ev.matches, 'query:', ev.media);
  // Styles are already consistent here, the restyle ran before delivery.
}
dark.addEventListener('change', onSchemeChange);
dark.removeEventListener('change', onSchemeChange);

// Handler property:
dark.onchange = (ev) => applyTheme(ev.matches ? 'dark' : 'light');
dark.onchange = null;

// Legacy aliases (pre-2020 API, still common in libraries):
dark.addListener(onSchemeChange);
dark.removeListener(onSchemeChange);

// Typical responsive-layout wiring, the listener keeps firing even if the
// app never stores the list anywhere (listening lists are realm-pinned):
matchMedia('(max-width: 700px)').addEventListener('change', (ev) => {
  document.body.classList.toggle('compact', ev.matches);
});

// Flipping the scheme setting fires 'change' on every affected list:
bro.settings.set('appearance.colorScheme', 'dark');   // or 'light' / 'system'

// ---------------------------------------------------------------------------
// Headless testing
// ---------------------------------------------------------------------------

// resize() re-evaluates synchronously, assert right after:
const narrow = matchMedia('(max-width: 500px)');
resize(400, 300);
narrow.matches;              // true, and 'change' has already fired
resize(1024, 768);

// Scheme flips deliver on the next flush (after the restyle):
bro.settings.set('appearance.colorScheme', 'dark');
flush();                     // 'change' events for prefers-color-scheme lists

// Regression: document lifecycle ordering.
//
// Layout must be complete, and document.readyState must be a real progressing
// value, by the time DOMContentLoaded/load handlers run. Apps measure DOM
// geometry (clientWidth, getBoundingClientRect) in those handlers — the
// universal, correct web idiom. Before this fix bro dispatched both events
// before its first layout pass and hardcoded readyState to "complete", so
// geometry reads in those handlers returned pre-layout fallbacks.
//
// The capture lives in tests/test_app/index.html (window.__lifecycle).
// #root is a block div filling the 1920px-wide test_app body: with layout it
// measures 1920; without layout clientWidth returns the 300px element fallback.

const lc = window.__lifecycle;
assert(lc, '__lifecycle capture present (test_app inline script ran)');

// During script execution the document is still loading — no layout exists.
assert(lc.scriptEval.readyState === 'loading',
       'readyState during script eval is "loading", got ' + lc.scriptEval.readyState);

// DOMContentLoaded: readyState "interactive", layout complete.
assert(lc.domContentLoaded, 'DOMContentLoaded handler fired');
assert(lc.domContentLoaded.readyState === 'interactive',
       'readyState at DOMContentLoaded is "interactive", got ' + lc.domContentLoaded.readyState);
assert(lc.domContentLoaded.rootWidth === 1920,
       '#root has real layout width (1920) at DOMContentLoaded, got ' + lc.domContentLoaded.rootWidth);

// load: readyState "complete", layout complete.
assert(lc.load, 'load handler fired');
assert(lc.load.readyState === 'complete',
       'readyState at load is "complete", got ' + lc.load.readyState);
assert(lc.load.rootWidth === 1920,
       '#root has real layout width (1920) at load, got ' + lc.load.rootWidth);

// Test scripts run after load — readyState has settled at "complete".
assert(document.readyState === 'complete',
       'readyState is "complete" in test script, got ' + document.readyState);

// ============================================================================
// Scrollbar regression tests
//
// Tests the three bugs fixed in the scrollbar implementation:
//   1. overflow:hidden must NOT render scrollbars (only scroll/auto should)
//   2. Nested scrollable elements: deepest element takes priority
//   3. Shadow DOM: host with overflow:hidden should not show scrollbar,
//      slotted child with overflow:auto should scroll correctly
// ============================================================================

var passed = 0;
var failed = 0;

function check(name, condition) {
    if (condition) {
        console.log('  PASS: ' + name);
        passed++;
    } else {
        console.log('  FAIL: ' + name);
        failed++;
    }
}

// Let layout settle
flush();

// -------------------------------------------------------------------------
// Test 1: overflow:hidden must NOT be scrollable via wheel
//
// BUG: overflowClips() returned true for "hidden", causing scrollbars to
// render and hit-test for hidden-overflow elements.
// FIX: use overflowScrollable() which only matches "scroll"/"auto".
// -------------------------------------------------------------------------
console.log('');
console.log('Test 1: overflow:hidden element');
(function() {
    var box = document.getElementById('hidden-box');
    var rect = box.getBoundingClientRect();
    var cx = rect.x + rect.width / 2;
    var cy = rect.y + rect.height / 2;

    check('hidden-box scrollTop starts at 0', box.scrollTop === 0);

    // Try to scroll via wheel — should NOT scroll (overflow:hidden)
    wheel(cx, cy, -5);
    flush();
    check('hidden-box scrollTop unchanged after wheel down', box.scrollTop === 0);

    // Try scrolling up too
    wheel(cx, cy, 5);
    flush();
    check('hidden-box scrollTop unchanged after wheel up', box.scrollTop === 0);
})();

// -------------------------------------------------------------------------
// Test 2: overflow:auto SHOULD be scrollable via wheel
// -------------------------------------------------------------------------
console.log('');
console.log('Test 2: overflow:auto element');
(function() {
    var box = document.getElementById('auto-box');
    var rect = box.getBoundingClientRect();
    var cx = rect.x + rect.width / 2;
    var cy = rect.y + rect.height / 2;

    check('auto-box scrollTop starts at 0', box.scrollTop === 0);

    // Scroll down
    wheel(cx, cy, -2);
    flush();
    var scrollAfterDown = box.scrollTop;
    check('auto-box scrolls down via wheel', scrollAfterDown > 0);

    // Scroll back up
    wheel(cx, cy, 2);
    flush();
    check('auto-box scrolls up via wheel', box.scrollTop < scrollAfterDown);

    // Reset for later tests
    box.scrollTop = 0;
    flush();
})();

// -------------------------------------------------------------------------
// Test 3: overflow:scroll SHOULD be scrollable via wheel
// -------------------------------------------------------------------------
console.log('');
console.log('Test 3: overflow:scroll element');
(function() {
    var box = document.getElementById('scroll-box');
    var rect = box.getBoundingClientRect();
    var cx = rect.x + rect.width / 2;
    var cy = rect.y + rect.height / 2;

    check('scroll-box scrollTop starts at 0', box.scrollTop === 0);

    wheel(cx, cy, -2);
    flush();
    check('scroll-box scrolls down via wheel', box.scrollTop > 0);

    // Reset
    box.scrollTop = 0;
    flush();
})();

// -------------------------------------------------------------------------
// Test 4: Nested scrollable — inner element takes priority
//
// BUG: findElementScrollbarHit checked self BEFORE children, so the outer
// ancestor would consume scrollbar hits meant for the inner element.
// FIX: recurse into children first, return deepest match.
// -------------------------------------------------------------------------
console.log('');
console.log('Test 4: Nested scrollable elements (deepest-match priority)');
(function() {
    var outer = document.getElementById('outer-scroll');
    var inner = document.getElementById('inner-scroll');
    var innerRect = inner.getBoundingClientRect();
    var cx = innerRect.x + innerRect.width / 2;
    var cy = innerRect.y + innerRect.height / 2;

    outer.scrollTop = 0;
    inner.scrollTop = 0;
    flush();

    check('outer scrollTop starts at 0', outer.scrollTop === 0);
    check('inner scrollTop starts at 0', inner.scrollTop === 0);

    // Wheel over the inner element — MUST scroll inner, NOT outer
    wheel(cx, cy, -2);
    flush();

    check('inner scrollTop increased after wheel', inner.scrollTop > 0);
    check('outer scrollTop unchanged (inner consumed scroll)', outer.scrollTop === 0);

    // Now wheel over the outer area (above inner box)
    var outerRect = outer.getBoundingClientRect();
    wheel(outerRect.x + outerRect.width / 2, outerRect.y + 10, -2);
    flush();

    check('outer scrollTop increased when wheeling over outer area', outer.scrollTop > 0);
})();

// -------------------------------------------------------------------------
// Test 5: Shadow DOM — host with overflow:hidden, slotted child with overflow:auto
//
// BUG: overflowClips() matched "hidden" on the shadow host, causing a
// scrollbar to render at host level (window-level position).
// FIX: only scroll/auto get scrollbars via overflowScrollable().
// -------------------------------------------------------------------------
console.log('');
console.log('Test 5: Shadow DOM scrollbar scoping');
(function() {
    var host = document.getElementById('shadow-host');
    var inner = document.getElementById('shadow-inner-scrollable');
    var innerRect = inner.getBoundingClientRect();
    var cx = innerRect.x + innerRect.width / 2;
    var cy = innerRect.y + innerRect.height / 2;

    host.scrollTop = 0;
    inner.scrollTop = 0;
    flush();

    check('shadow host scrollTop starts at 0', host.scrollTop === 0);
    check('shadow inner scrollTop starts at 0', inner.scrollTop === 0);

    // Wheel over the slotted scrollable element
    wheel(cx, cy, -2);
    flush();

    check('shadow inner scrolls via wheel', inner.scrollTop > 0);
    check('shadow host not scrolled (overflow:hidden)', host.scrollTop === 0);

    // Wheel over the host area outside the inner element
    var hostRect = host.getBoundingClientRect();
    var prevHostScroll = host.scrollTop;
    wheel(hostRect.x + hostRect.width / 2, hostRect.y + 5, -2);
    flush();

    check('shadow host still not scrolled via wheel on host area',
          host.scrollTop === prevHostScroll);
})();

// -------------------------------------------------------------------------
// Test 6: Coordinate-based click goes through full engine pipeline
// -------------------------------------------------------------------------
console.log('');
console.log('Test 6: click(x,y) engine pipeline');
(function() {
    var box = document.getElementById('auto-box');
    var clickFired = false;
    var mousedownFired = false;
    box.addEventListener('click', function() { clickFired = true; });
    box.addEventListener('mousedown', function() { mousedownFired = true; });

    var rect = box.getBoundingClientRect();
    click(rect.x + 10, rect.y + 10);

    check('click(x,y) fires mousedown', mousedownFired);
    check('click(x,y) fires click', clickFired);
})();

// -------------------------------------------------------------------------
// Test 7: mouseDown/mouseUp fire separate events
// -------------------------------------------------------------------------
console.log('');
console.log('Test 7: mouseDown/mouseUp events');
(function() {
    var box = document.getElementById('scroll-box');
    var events = [];
    box.addEventListener('mousedown', function() { events.push('down'); });
    box.addEventListener('mouseup', function() { events.push('up'); });
    box.addEventListener('click', function() { events.push('click'); });

    var rect = box.getBoundingClientRect();
    mouseDown(rect.x + 10, rect.y + 10);
    mouseUp(rect.x + 10, rect.y + 10);

    check('mouseDown fires mousedown', events.indexOf('down') >= 0);
    check('mouseUp fires mouseup', events.indexOf('up') >= 0);
    check('mouseDown+mouseUp fires click', events.indexOf('click') >= 0);
    check('event order: down, up, click',
          events.length >= 3 &&
          events[0] === 'down' && events[1] === 'up' && events[2] === 'click');
})();

// -------------------------------------------------------------------------
// Test 8: mouseMove fires mousemove
// -------------------------------------------------------------------------
console.log('');
console.log('Test 8: mouseMove events');
(function() {
    var box = document.getElementById('auto-box');
    var moveFired = false;
    box.addEventListener('mousemove', function() { moveFired = true; });

    var rect = box.getBoundingClientRect();
    mouseMove(rect.x + 10, rect.y + 10);

    check('mouseMove fires mousemove event', moveFired);
})();

// -------------------------------------------------------------------------
// Summary
// -------------------------------------------------------------------------
console.log('');
console.log('========================================');
console.log('Results: ' + passed + ' passed, ' + failed + ' failed');
console.log('========================================');

screenshot('scrollbar-test.png');

assert(failed === 0, failed + ' test(s) failed');

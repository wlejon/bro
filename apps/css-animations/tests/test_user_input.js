// Headless tests for CSS animations app — user input (click, hover, mousedown, etc.)
// Run: bro-headless apps/css-animations apps/css-animations/tests/test_user_input.js
//
// Tests exercise the CSS transitions section which uses inline onclick handlers
// to toggle an "active" class on child elements.

var passed = 0;
var failed = 0;

function test(name, fn) {
    try {
        fn();
        passed++;
        console.log('PASS: ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL: ' + name + ' - ' + e.message);
    }
}

function assertEqual(a, b, msg) {
    if (a !== b) throw new Error((msg || '') + ' expected "' + b + '" but got "' + a + '"');
}

function assertTrue(cond, msg) {
    if (!cond) throw new Error(msg || 'assertion failed');
}

function assertFalse(cond, msg) {
    if (cond) throw new Error(msg || 'expected false but got true');
}

// Wait for initial layout
flush();

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Get center coordinates of an element by selector
function getCenter(selector) {
    var el = document.querySelector(selector);
    if (!el) throw new Error('getCenter: no element matches "' + selector + '"');
    var rect = el.getBoundingClientRect();
    return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
}

// Check if an element has a CSS class
function hasClass(selector, cls) {
    var el = document.querySelector(selector);
    if (!el) return false;
    return el.classList.contains(cls);
}

// ---------------------------------------------------------------------------
// Test 1: DOM structure — transition cards exist
// ---------------------------------------------------------------------------
test('transition cards are present in DOM', function() {
    var cards = document.querySelectorAll('.transition-card');
    assertTrue(cards.length === 8, 'expected 8 transition cards, got ' + cards.length);
});

// ---------------------------------------------------------------------------
// Test 2: Transition boxes have correct initial classes (no "active")
// ---------------------------------------------------------------------------
test('transition boxes start without active class', function() {
    var selectors = ['.t-color', '.t-scale', '.t-rotate', '.t-shadow',
                     '.t-multi', '.t-opacity', '.t-size', '.t-cubic'];
    for (var i = 0; i < selectors.length; i++) {
        assertFalse(hasClass(selectors[i], 'active'),
            selectors[i] + ' should not have active class initially');
    }
});

// ---------------------------------------------------------------------------
// Test 3: element.click() toggles active class via inline onclick
// ---------------------------------------------------------------------------
test('element.click() on transition card toggles active class on child', function() {
    var card = document.querySelector('.transition-card');
    assertTrue(card !== null, 'should find a transition card');
    card.click();
    flush();
    assertTrue(hasClass('.t-color', 'active'),
        '.t-color should have active class after click');
});

// ---------------------------------------------------------------------------
// Test 4: Second click removes the active class (toggle off)
// ---------------------------------------------------------------------------
test('second element.click() toggles active class off', function() {
    var card = document.querySelector('.transition-card');
    card.click();
    flush();
    assertFalse(hasClass('.t-color', 'active'),
        '.t-color should NOT have active class after second click');
});

// ---------------------------------------------------------------------------
// Test 5: Coordinate-based click() toggles the color transition box
// ---------------------------------------------------------------------------
test('coordinate-based click() on t-color card toggles active', function() {
    // First ensure it starts inactive
    assertFalse(hasClass('.t-color', 'active'), 't-color should start inactive');

    var pos = getCenter('.t-color');
    click(pos.x, pos.y);
    assertTrue(hasClass('.t-color', 'active'),
        't-color should have active class after coordinate click');

    // Click again to toggle off
    click(pos.x, pos.y);
    assertFalse(hasClass('.t-color', 'active'),
        't-color should lose active class after second coordinate click');
});

// ---------------------------------------------------------------------------
// Test 6: mouseDown + mouseUp sequence produces click event
// ---------------------------------------------------------------------------
test('mouseDown + mouseUp on same element triggers onclick handler', function() {
    assertFalse(hasClass('.t-scale', 'active'), 't-scale should start inactive');

    var pos = getCenter('.t-scale');
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
    assertTrue(hasClass('.t-scale', 'active'),
        't-scale should have active class after mouseDown+mouseUp');

    // Clean up
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 7: mouseDown then mouseUp on DIFFERENT element — no click
// ---------------------------------------------------------------------------
test('mouseDown and mouseUp on different elements does not trigger click', function() {
    assertFalse(hasClass('.t-rotate', 'active'), 't-rotate should start inactive');

    var pos1 = getCenter('.t-color');
    var pos2 = getCenter('.t-rotate');
    mouseDown(pos1.x, pos1.y);
    mouseUp(pos2.x, pos2.y);
    // t-rotate should NOT toggle because mousedown was on a different element
    assertFalse(hasClass('.t-rotate', 'active'),
        't-rotate should NOT activate when mouseDown was on different element');
});

// ---------------------------------------------------------------------------
// Test 8: Click each transition card individually
// ---------------------------------------------------------------------------
test('click each transition card toggles its child box', function() {
    var pairs = [
        { card: '.transition-card:nth-child(1)', box: '.t-color' },
        { card: '.transition-card:nth-child(2)', box: '.t-scale' },
        { card: '.transition-card:nth-child(3)', box: '.t-rotate' },
        { card: '.transition-card:nth-child(4)', box: '.t-shadow' },
        { card: '.transition-card:nth-child(5)', box: '.t-multi' },
        { card: '.transition-card:nth-child(6)', box: '.t-opacity' },
        { card: '.transition-card:nth-child(7)', box: '.t-size' },
        { card: '.transition-card:nth-child(8)', box: '.t-cubic' }
    ];

    // Use coordinate-based clicks to test hit testing + event bubbling
    for (var i = 0; i < pairs.length; i++) {
        var boxSel = pairs[i].box;
        assertFalse(hasClass(boxSel, 'active'), boxSel + ' should start inactive');

        var pos = getCenter(boxSel);
        click(pos.x, pos.y);

        assertTrue(hasClass(boxSel, 'active'),
            boxSel + ' should be active after click');

        // Toggle off
        click(pos.x, pos.y);
        assertFalse(hasClass(boxSel, 'active'),
            boxSel + ' should be inactive after second click');
    }
});

// ---------------------------------------------------------------------------
// Test 9: Hover (mouseMove) triggers mouseover/mouseenter
// ---------------------------------------------------------------------------
test('mouseMove over element triggers hover state tracking', function() {
    var eventsReceived = [];
    var box = document.querySelector('.t-color');

    box.addEventListener('mouseover', function() { eventsReceived.push('mouseover'); });
    box.addEventListener('mouseenter', function() { eventsReceived.push('mouseenter'); });
    box.addEventListener('mouseout', function() { eventsReceived.push('mouseout'); });
    box.addEventListener('mouseleave', function() { eventsReceived.push('mouseleave'); });

    // Move to the element
    var pos = getCenter('.t-color');
    mouseMove(0, 0);  // start off-element
    mouseMove(pos.x, pos.y);  // move onto element

    assertTrue(eventsReceived.indexOf('mouseover') >= 0,
        'should receive mouseover event');
    assertTrue(eventsReceived.indexOf('mouseenter') >= 0,
        'should receive mouseenter event');

    // Move away
    eventsReceived = [];
    mouseMove(0, 0);
    assertTrue(eventsReceived.indexOf('mouseout') >= 0,
        'should receive mouseout event');
    assertTrue(eventsReceived.indexOf('mouseleave') >= 0,
        'should receive mouseleave event');
});

// ---------------------------------------------------------------------------
// Test 10: mousedown event fires on element
// ---------------------------------------------------------------------------
test('mousedown event fires on target element', function() {
    var received = false;
    var box = document.querySelector('.t-scale');
    box.addEventListener('mousedown', function() { received = true; });

    var pos = getCenter('.t-scale');
    mouseDown(pos.x, pos.y);
    assertTrue(received, 'mousedown event should fire');
    mouseUp(pos.x, pos.y);

    // Clean up toggle
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 11: mouseup event fires on element
// ---------------------------------------------------------------------------
test('mouseup event fires on target element', function() {
    var received = false;
    var box = document.querySelector('.t-rotate');
    box.addEventListener('mouseup', function() { received = true; });

    var pos = getCenter('.t-rotate');
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
    assertTrue(received, 'mouseup event should fire');

    // Clean up toggle
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 12: click event fires (separate from inline onclick)
// ---------------------------------------------------------------------------
test('click event fires via addEventListener', function() {
    var clickCount = 0;
    var card = document.querySelectorAll('.transition-card')[3]; // t-shadow card
    card.addEventListener('click', function() { clickCount++; });

    var pos = getCenter('.t-shadow');
    click(pos.x, pos.y);
    assertTrue(clickCount >= 1, 'click listener should fire (count=' + clickCount + ')');

    // Clean up toggle
    click(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 13: Event bubbling — click on child bubbles to parent
// ---------------------------------------------------------------------------
test('click on child box bubbles to parent transition-card', function() {
    var parentClicked = false;
    var cards = document.querySelectorAll('.transition-card');
    var card = cards[4]; // t-multi parent card
    card.addEventListener('click', function() { parentClicked = true; });

    var pos = getCenter('.t-multi');
    click(pos.x, pos.y);
    assertTrue(parentClicked, 'click should bubble from child to parent');

    // Clean up toggle
    click(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 14: classList.toggle works correctly
// ---------------------------------------------------------------------------
test('classList.toggle adds and removes class correctly', function() {
    var el = document.querySelector('.t-color');
    var className = el.className;

    // Should not have 'active' initially (we cleaned up earlier)
    assertFalse(el.classList.contains('active'), 'should start without active');

    // Toggle on
    var result = el.classList.toggle('active');
    assertTrue(result, 'toggle should return true when adding');
    assertTrue(el.classList.contains('active'), 'should have active after toggle');

    // Toggle off
    result = el.classList.toggle('active');
    assertFalse(result, 'toggle should return false when removing');
    assertFalse(el.classList.contains('active'), 'should not have active after second toggle');
});

// ---------------------------------------------------------------------------
// Test 15: classList.add / classList.remove / classList.contains
// ---------------------------------------------------------------------------
test('classList.add, remove, contains work correctly', function() {
    var el = document.querySelector('.t-scale');

    assertFalse(el.classList.contains('test-class'), 'should not have test-class initially');

    el.classList.add('test-class');
    assertTrue(el.classList.contains('test-class'), 'should have test-class after add');

    el.classList.remove('test-class');
    assertFalse(el.classList.contains('test-class'), 'should not have test-class after remove');
});

// ---------------------------------------------------------------------------
// Test 16: inline onclick "this" refers to the element with the attribute
// ---------------------------------------------------------------------------
test('inline onclick "this" correctly refers to the attributed element', function() {
    // The onclick is on the card: this.querySelector('.t-color') should find child
    // We verify by checking that clicking the card's child still toggles correctly
    var pos = getCenter('.t-color');
    assertFalse(hasClass('.t-color', 'active'), 'should start inactive');

    click(pos.x, pos.y);
    assertTrue(hasClass('.t-color', 'active'),
        'inline onclick "this" should correctly reference card element to querySelector child');

    // Clean up
    click(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 17: mousemove event fires with correct coordinates
// ---------------------------------------------------------------------------
test('mousemove event carries coordinate properties', function() {
    var eventData = null;
    var box = document.querySelector('.t-cubic');
    box.addEventListener('mousemove', function(e) {
        eventData = { clientX: e.clientX, clientY: e.clientY };
    });

    var pos = getCenter('.t-cubic');
    mouseMove(pos.x, pos.y);

    assertTrue(eventData !== null, 'mousemove listener should fire');
    assertTrue(typeof eventData.clientX === 'number', 'clientX should be a number');
    assertTrue(typeof eventData.clientY === 'number', 'clientY should be a number');
});

// ---------------------------------------------------------------------------
// Test 18: Rapid toggle (click-click-click) results in correct final state
// ---------------------------------------------------------------------------
test('rapid toggle produces correct final state', function() {
    assertFalse(hasClass('.t-opacity', 'active'), 'start inactive');

    var pos = getCenter('.t-opacity');
    click(pos.x, pos.y); // on
    click(pos.x, pos.y); // off
    click(pos.x, pos.y); // on

    assertTrue(hasClass('.t-opacity', 'active'),
        'after 3 clicks (odd count) should be active');

    click(pos.x, pos.y); // off
    assertFalse(hasClass('.t-opacity', 'active'),
        'after 4 clicks (even count) should be inactive');
});

// ---------------------------------------------------------------------------
// Test 19: mousedown event has correct button property
// ---------------------------------------------------------------------------
test('mousedown event has button=0 for left click', function() {
    var buttonVal = -1;
    var box = document.querySelector('.t-color');
    box.addEventListener('mousedown', function(e) { buttonVal = e.button; });

    var pos = getCenter('.t-color');
    mouseDown(pos.x, pos.y, 0);
    assertTrue(buttonVal === 0, 'mousedown button should be 0 for left click, got ' + buttonVal);
    mouseUp(pos.x, pos.y, 0);

    // Clean up toggle
    mouseDown(pos.x, pos.y, 0);
    mouseUp(pos.x, pos.y, 0);
});

// ---------------------------------------------------------------------------
// Test 20: querySelector works inside inline handler context
// ---------------------------------------------------------------------------
test('querySelector from inline handler finds child elements', function() {
    // This tests the specific pattern used: this.querySelector('.t-color')
    // by verifying all 8 boxes can be toggled via their parent card onclick
    var selectors = ['.t-color', '.t-scale', '.t-rotate', '.t-shadow',
                     '.t-multi', '.t-opacity', '.t-size', '.t-cubic'];

    for (var i = 0; i < selectors.length; i++) {
        var sel = selectors[i];
        assertFalse(hasClass(sel, 'active'), sel + ' should start inactive');

        // Use element.click() on the parent card
        var box = document.querySelector(sel);
        var card = box.parentElement;
        assertTrue(card !== null, sel + ' should have a parent');
        card.click();
        flush();

        assertTrue(hasClass(sel, 'active'),
            sel + ' should be active after card.click() — inline handler querySelector works');

        // Toggle off
        card.click();
        flush();
        assertFalse(hasClass(sel, 'active'), sel + ' should be toggled back off');
    }
});

// ---------------------------------------------------------------------------
// Test 21: Event listener on document captures bubbled events
// ---------------------------------------------------------------------------
test('click events bubble to document level', function() {
    var docClicked = false;
    document.addEventListener('click', function() { docClicked = true; });

    var pos = getCenter('.t-color');
    click(pos.x, pos.y);
    assertTrue(docClicked, 'click should bubble to document');

    // Clean up toggle
    click(pos.x, pos.y);
});

// ---------------------------------------------------------------------------
// Test 22: Double-click detection
// ---------------------------------------------------------------------------
test('two rapid clicks produce dblclick event', function() {
    var dblClickReceived = false;
    var box = document.querySelector('.t-size');
    box.addEventListener('dblclick', function() { dblClickReceived = true; });

    var pos = getCenter('.t-size');
    click(pos.x, pos.y);
    click(pos.x, pos.y);
    assertTrue(dblClickReceived, 'dblclick event should fire after two rapid clicks');
});

// ---------------------------------------------------------------------------
// Test 23: Class toggle marks document dirty (triggers re-layout)
// ---------------------------------------------------------------------------
test('classList.toggle marks document dirty for re-layout', function() {
    var el = document.querySelector('.t-size');
    assertFalse(el.classList.contains('active'), 'start without active');

    el.classList.toggle('active');
    // After toggling, the document should be marked dirty
    // flush() should resolve it
    flush();

    assertTrue(el.classList.contains('active'), 'class should still be active after flush');

    // Clean up
    el.classList.toggle('active');
    flush();
});

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('');
console.log('Results: ' + passed + ' passed, ' + failed + ' failed out of ' + (passed + failed));

if (failed > 0) {
    assert(false, failed + ' test(s) failed');
}

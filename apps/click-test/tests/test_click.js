// Headless click tests — pure class toggling, no CSS transitions/animations.
// Run: bro-headless apps/click-test apps/click-test/tests/test_click.js

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

function assertTrue(cond, msg) {
    if (!cond) throw new Error(msg || 'expected true');
}
function assertFalse(cond, msg) {
    if (cond) throw new Error(msg || 'expected false');
}
function assertEqual(a, b, msg) {
    if (a !== b) throw new Error((msg || '') + ' expected "' + b + '" got "' + a + '"');
}

flush();

function getCenter(sel) {
    var el = document.querySelector(sel);
    if (!el) throw new Error('no element: ' + sel);
    var r = el.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
}

function isActive(sel) {
    return document.querySelector(sel).classList.contains('active');
}

// ---------------------------------------------------------------------------
// 1. Initial state
// ---------------------------------------------------------------------------
test('all boxes start without active', function() {
    assertFalse(isActive('#box1'));
    assertFalse(isActive('#box2'));
    assertFalse(isActive('#box3'));
    assertFalse(isActive('#box4'));
});

// ---------------------------------------------------------------------------
// 2. Inline onclick — element.click() on parent card
// ---------------------------------------------------------------------------
test('element.click() on card with inline onclick toggles child', function() {
    var card = document.querySelector('#box1').parentElement;
    card.click();
    flush();
    assertTrue(isActive('#box1'), 'box1 should be active');
    card.click();
    flush();
    assertFalse(isActive('#box1'), 'box1 should be inactive');
});

// ---------------------------------------------------------------------------
// 3. Inline onclick — coordinate click on parent card
// ---------------------------------------------------------------------------
test('coordinate click() on card with inline onclick toggles child', function() {
    var pos = getCenter('#box1');
    click(pos.x, pos.y);
    assertTrue(isActive('#box1'), 'box1 active after coord click');
    click(pos.x, pos.y);
    assertFalse(isActive('#box1'), 'box1 inactive after second coord click');
});

// ---------------------------------------------------------------------------
// 4. addEventListener — element.click() on parent card
// ---------------------------------------------------------------------------
test('element.click() on card with addEventListener toggles child', function() {
    var card = document.getElementById('card2');
    card.click();
    flush();
    assertTrue(isActive('#box2'), 'box2 should be active');
    card.click();
    flush();
    assertFalse(isActive('#box2'), 'box2 should be inactive');
});

// ---------------------------------------------------------------------------
// 5. addEventListener — coordinate click on parent card
// ---------------------------------------------------------------------------
test('coordinate click() on card2 toggles box2', function() {
    var pos = getCenter('#box2');
    click(pos.x, pos.y);
    assertTrue(isActive('#box2'), 'box2 active after coord click');
    click(pos.x, pos.y);
    assertFalse(isActive('#box2'), 'box2 inactive after second coord click');
});

// ---------------------------------------------------------------------------
// 6. addEventListener on box directly — element.click()
// ---------------------------------------------------------------------------
test('element.click() directly on box3 toggles it', function() {
    document.getElementById('box3').click();
    flush();
    assertTrue(isActive('#box3'), 'box3 should be active');
    document.getElementById('box3').click();
    flush();
    assertFalse(isActive('#box3'), 'box3 should be inactive');
});

// ---------------------------------------------------------------------------
// 7. addEventListener on box directly — coordinate click
// ---------------------------------------------------------------------------
test('coordinate click() on box3 toggles it', function() {
    var pos = getCenter('#box3');
    click(pos.x, pos.y);
    assertTrue(isActive('#box3'), 'box3 active');
    click(pos.x, pos.y);
    assertFalse(isActive('#box3'), 'box3 inactive');
});

// ---------------------------------------------------------------------------
// 8. Bubbling — click child, inline onclick on parent fires
// ---------------------------------------------------------------------------
test('click on box4 bubbles to parent inline onclick', function() {
    var pos = getCenter('#box4');
    click(pos.x, pos.y);
    assertTrue(isActive('#box4'), 'box4 active via bubbling');
    click(pos.x, pos.y);
    assertFalse(isActive('#box4'), 'box4 inactive after second click');
});

// ---------------------------------------------------------------------------
// 9. mouseDown + mouseUp = click
// ---------------------------------------------------------------------------
test('mouseDown + mouseUp produces click', function() {
    var pos = getCenter('#box1');
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
    assertTrue(isActive('#box1'), 'box1 active after mouseDown+mouseUp');
    mouseDown(pos.x, pos.y);
    mouseUp(pos.x, pos.y);
    assertFalse(isActive('#box1'), 'box1 inactive after second pair');
});

// ---------------------------------------------------------------------------
// 10. mouseDown + mouseUp on different elements = no click
// ---------------------------------------------------------------------------
test('mouseDown/mouseUp on different elements does not click', function() {
    assertFalse(isActive('#box1'));
    var p1 = getCenter('#box1');
    var p2 = getCenter('#box2');
    mouseDown(p1.x, p1.y);
    mouseUp(p2.x, p2.y);
    assertFalse(isActive('#box1'), 'box1 should not toggle');
    assertFalse(isActive('#box2'), 'box2 should not toggle');
});

// ---------------------------------------------------------------------------
// 11. Computed style changes after toggle
// ---------------------------------------------------------------------------
test('background-color changes after classList.toggle', function() {
    var bg1 = computedStyle('#box1', 'background-color');
    var pos = getCenter('#box1');
    click(pos.x, pos.y);
    var bg2 = computedStyle('#box1', 'background-color');
    assertTrue(bg1 !== bg2, 'bg should change: before=' + bg1 + ' after=' + bg2);
    click(pos.x, pos.y); // clean up
});

// ---------------------------------------------------------------------------
// 12. Multiple rapid toggles
// ---------------------------------------------------------------------------
test('5 clicks = active (odd count)', function() {
    var pos = getCenter('#box3');
    for (var i = 0; i < 5; i++) click(pos.x, pos.y);
    assertTrue(isActive('#box3'), 'odd clicks = active');
    click(pos.x, pos.y); // clean up
});

// ---------------------------------------------------------------------------
// 13. Event object properties
// ---------------------------------------------------------------------------
test('click event has correct button and type', function() {
    var evtData = null;
    var box = document.getElementById('box3');
    box.addEventListener('click', function(e) {
        evtData = { type: e.type, button: e.button };
    });
    var pos = getCenter('#box3');
    click(pos.x, pos.y);
    assertTrue(evtData !== null, 'event should fire');
    assertEqual(evtData.type, 'click', 'type');
    assertEqual(evtData.button, 0, 'button');
    click(pos.x, pos.y); // clean up
});

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('');
console.log('Results: ' + passed + ' passed, ' + failed + ' failed out of ' + (passed + failed));
if (failed > 0) assert(false, failed + ' test(s) failed');

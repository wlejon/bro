// Headless click tests — verifies click handling across different patterns.
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

// --- A. Inline onclick on card ---
test('A1: inline onclick toggles child', function() {
    assertFalse(isActive('#a1'));
    var pos = getCenter('#a1');
    click(pos.x, pos.y);
    assertTrue(isActive('#a1'), 'a1 should be active');
    click(pos.x, pos.y);
    assertFalse(isActive('#a1'), 'a1 should be inactive');
});

test('A2: inline onclick toggles child', function() {
    var pos = getCenter('#a2');
    click(pos.x, pos.y);
    assertTrue(isActive('#a2'));
    click(pos.x, pos.y);
    assertFalse(isActive('#a2'));
});

// --- B. addEventListener on card ---
test('B1: addEventListener on card toggles child', function() {
    var pos = getCenter('#b1');
    click(pos.x, pos.y);
    assertTrue(isActive('#b1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#b1'));
});

test('B2: addEventListener on card toggles child', function() {
    var pos = getCenter('#b2');
    click(pos.x, pos.y);
    assertTrue(isActive('#b2'));
    click(pos.x, pos.y);
    assertFalse(isActive('#b2'));
});

// --- C. addEventListener directly on box ---
test('C1: listener on box toggles it', function() {
    var pos = getCenter('#c1');
    click(pos.x, pos.y);
    assertTrue(isActive('#c1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#c1'));
});

test('C2: listener on box toggles it', function() {
    var pos = getCenter('#c2');
    click(pos.x, pos.y);
    assertTrue(isActive('#c2'));
    click(pos.x, pos.y);
    assertFalse(isActive('#c2'));
});

// --- D. Nested elements ---
test('D1: inline onclick, 3-deep nesting', function() {
    var pos = getCenter('#d1');
    click(pos.x, pos.y);
    assertTrue(isActive('#d1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#d1'));
});

test('D2: addEventListener, 3-deep nesting', function() {
    var pos = getCenter('#d2');
    click(pos.x, pos.y);
    assertTrue(isActive('#d2'));
    click(pos.x, pos.y);
    assertFalse(isActive('#d2'));
});

// --- E. Mixed inline + addEventListener ---
test('E1: both handlers fire', function() {
    var pos = getCenter('#e1');
    click(pos.x, pos.y);
    assertTrue(isActive('#e1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#e1'));
});

// --- F. onclick directly on box ---
test('F1: onclick on box itself', function() {
    var pos = getCenter('#f1');
    click(pos.x, pos.y);
    assertTrue(isActive('#f1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#f1'));
});

test('F2: onclick on box itself', function() {
    var pos = getCenter('#f2');
    click(pos.x, pos.y);
    assertTrue(isActive('#f2'));
    click(pos.x, pos.y);
    assertFalse(isActive('#f2'));
});

// --- G. Text toggle ---
test('G1: text content toggles', function() {
    var g1 = document.getElementById('g1');
    assertEqual(g1.textContent, 'OFF');
    var pos = getCenter('#g1-card');
    click(pos.x, pos.y);
    assertEqual(g1.textContent, 'ON');
    click(pos.x, pos.y);
    assertEqual(g1.textContent, 'OFF');
});

// --- H. Inline onclick with event param ---
test('H1: onclick with event object', function() {
    var pos = getCenter('#h1');
    click(pos.x, pos.y);
    assertTrue(isActive('#h1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#h1'));
});

// --- I. CSS Transitions ---
test('I1: bg-color transition toggles', function() {
    var pos = getCenter('#i1');
    click(pos.x, pos.y);
    assertTrue(isActive('#i1'));
    click(pos.x, pos.y);
    assertFalse(isActive('#i1'));
});

test('I5: bg-color transition (addEventListener)', function() {
    var pos = getCenter('#i5');
    click(pos.x, pos.y);
    assertTrue(isActive('#i5'));
    click(pos.x, pos.y);
    assertFalse(isActive('#i5'));
});

// ---------------------------------------------------------------------------
console.log('');
console.log('Results: ' + passed + ' passed, ' + failed + ' failed out of ' + (passed + failed));
if (failed > 0) assert(false, failed + ' test(s) failed');

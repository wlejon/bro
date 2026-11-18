// Invalid CSS selectors (including collapsed @view range tokens like 0.13-25)
// and handle ranges sent to agent-control commands must fail cleanly without
// matching <html> or delivering stray clicks to the page.

function runCtl() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 400; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}

var htmlClicks = 0;
document.documentElement.addEventListener('click', function() { htmlClicks++; });

document.body.innerHTML = '<div id="box"><button id="b1">One</button><button id="b2">Two</button></div>';
advanceTime(16);

// 1. Collapsed handle ranges fail with an explicit handle-range message.
var cases = [
    ['click', '0.13-25'],
    ['type', '0.13-25', 'hello'],
    ['set', '0.13-25', 'hello'],
    ['key', '0.13-25', 'enter'],
    ['scroll', '0.13-25'],
];
cases.forEach(function(argv) {
    var r = runCtl.apply(null, argv);
    assert(!r.ok, argv.join(' ') + ' fails instead of clicking <html>');
    assert(r.payload === '0.13-25 is a handle range; specify one handle from 0.13 to 0.25',
           'exact handle-range message for ' + argv[0] + ': ' + r.payload);
});
assert(htmlClicks === 0, 'no clicks landed on <html> after range handles (got ' + htmlClicks + ')');

// 2. Invalid CSS selectors passed to click fail with "bad selector" instead of matching <html>.
['123bad', 'div >', '#123', '[unclosed', '..bad'].forEach(function(badSel) {
    var r = runCtl('click', badSel);
    assert(!r.ok && r.payload.indexOf('bad selector') >= 0,
           'click ' + badSel + ' fails cleanly: ' + JSON.stringify(r));
});
assert(!runCtl('click', '.123').ok, 'click .123 also fails without clicking');
assert(htmlClicks === 0, 'no clicks landed on <html> after invalid selectors (got ' + htmlClicks + ')');

document.body.innerHTML = '';
console.log('test_control_handle_range.js PASSED');

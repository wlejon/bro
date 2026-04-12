// Test: scrollTop clamping for overflow:auto elements

flush();

var log = document.getElementById('log');
assert(log !== null, 'log exists');

// Add items to overflow
for (var i = 0; i < 30; i++) {
    var d = document.createElement('div');
    d.textContent = 'Line ' + i;
    log.appendChild(d);
}
flush();

var scrollHeight = log.scrollHeight;
var clientHeight = log.clientHeight;
var maxScroll = scrollHeight - clientHeight;

// Set scrollTop to scrollHeight (common "scroll to bottom" idiom)
log.scrollTop = scrollHeight;
assert(log.scrollTop <= maxScroll + 1,
    'scrollTop should clamp to max, got ' + log.scrollTop + ' max=' + maxScroll);
assert(log.scrollTop > 0, 'scrollTop should be positive when content overflows');

// Set scrollTop to negative
log.scrollTop = -100;
assert(log.scrollTop === 0, 'negative scrollTop should clamp to 0, got ' + log.scrollTop);

// Set scrollTop to a very large value
log.scrollTop = 999999;
assert(log.scrollTop <= maxScroll + 1,
    'huge scrollTop should clamp to max, got ' + log.scrollTop);

// Set scrollTop to a valid value
log.scrollTop = 50;
assert(Math.abs(log.scrollTop - 50) < 1, 'scrollTop=50 should stick, got ' + log.scrollTop);

console.log('PASS: scrollTop clamping');

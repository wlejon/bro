// Test video play() rejection on no source, seeking getter, and TimeRanges bounds checking

const video = document.createElement('video');
document.body.appendChild(video);

// 1. play() with no source returns a rejected promise with NotSupportedError
let playRejected = false;
let playErrorName = '';
const p = video.play();
assert(p && typeof p.then === 'function', 'play() returns a promise');
p.catch(function (err) {
    playRejected = true;
    playErrorName = err ? err.name : '';
});

// Flush microtasks
flush();

assert(playRejected, 'play() rejected when video has no source');
assert(playErrorName === 'NotSupportedError', 'rejection error is NotSupportedError, got: ' + playErrorName);

// 2. seeking getter returns boolean
assert(typeof video.seeking === 'boolean', 'seeking getter is a boolean');
assert(video.seeking === false, 'seeking is false when idle');

// 3. TimeRanges bounds checking
const ranges = video.buffered;
assert(ranges && typeof ranges.length === 'number', 'video.buffered returns TimeRanges');
assert(ranges.length === 0, 'empty ranges has length 0');

let startThrew = false;
try {
    ranges.start(0);
} catch (e) {
    startThrew = (e && e.name === 'IndexSizeError');
}
assert(startThrew, 'ranges.start(0) throws IndexSizeError when length is 0');

let endThrew = false;
try {
    ranges.end(0);
} catch (e) {
    endThrew = (e && e.name === 'IndexSizeError');
}
assert(endThrew, 'ranges.end(0) throws IndexSizeError when length is 0');

let negThrew = false;
try {
    ranges.start(-1);
} catch (e) {
    negThrew = (e && e.name === 'IndexSizeError');
}
assert(negThrew, 'ranges.start(-1) throws IndexSizeError');

console.log('OK — video play rejection, seeking, and TimeRanges bounds check passed');

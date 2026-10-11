// path.join / normalize / resolve / dirname / basename are native (brokit's
// path.cpp): a photo grid joins a folder with thousands of names, and the
// script versions cost ~7 us a join (resolve ~22 us). The limits here are
// ~10x under those.

const path = require('path');

const dir = path.sep === '/' ? '/home/someone/Pictures/Holiday 2024' : 'C:\\Users\\someone\\Pictures\\Holiday 2024';
const names = [];
for (let i = 0; i < 400; i++) names.push('IMG_' + (1000 + i) + '.jpg');

function costUs(fn) {
    for (let w = 0; w < 3; w++) for (const n of names) fn(n);
    let best = Infinity;
    for (let round = 0; round < 5; round++) {
        const t0 = perf.now();
        for (let r = 0; r < 10; r++) for (const n of names) fn(n);
        best = Math.min(best, (perf.now() - t0) * 1000 / (10 * names.length));
    }
    return best;
}

const join = costUs((n) => path.join(dir, n));
const normalize = costUs((n) => path.normalize(dir + '/./sub/../' + n));
const resolve = costUs((n) => path.resolve(dir, '..', n));
const dirname = costUs((n) => path.dirname(dir + path.sep + n));
const basename = costUs((n) => path.basename(dir + path.sep + n, '.jpg'));
console.log('test_path_speed: join ' + join.toFixed(2) + ' us, normalize ' + normalize.toFixed(2) +
    ' us, resolve ' + resolve.toFixed(2) + ' us, dirname ' + dirname.toFixed(2) + ' us, basename ' + basename.toFixed(2) + ' us');

// Correct, too.
assert(path.join(dir, names[0]) === dir + path.sep + names[0], 'join: ' + path.join(dir, names[0]));
assert(path.resolve(dir, '..', names[0]) === path.dirname(dir) + path.sep + names[0], 'resolve');

assert(join < 0.7, 'path.join costs ' + join.toFixed(2) + ' us a call (was ~7; limit 0.7)');
assert(normalize < 0.7, 'path.normalize costs ' + normalize.toFixed(2) + ' us (was ~4.5; limit 0.7)');
assert(resolve < 2.2, 'path.resolve costs ' + resolve.toFixed(2) + ' us (was ~22; limit 2.2)');
assert(dirname < 0.5 && basename < 0.5, 'dirname / basename under 0.5 us (were ~2)');

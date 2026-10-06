// Test for privileged desktop trust model
const privilegedNamespaces = ['displays', 'cred', 'seat', 'portal', 'sys', 'compositor', 'wl'];

for (const ns of privilegedNamespaces) {
    assert(typeof bro[ns] === 'object', `bro.${ns} object exists`);
    assert(bro[ns].available === false, `bro.${ns}.available is false for untrusted app`);

    let threw = false;
    let errMsg = '';
    try {
        // Attempting to invoke or access any function/method on the namespace throws
        if (typeof bro[ns].test === 'function') {
            bro[ns].test();
        } else {
            // Invoking the stub namespace directly
            bro[ns]();
        }
    } catch (e) {
        threw = true;
        errMsg = e.message;
    }
    assert(threw, `calling bro.${ns} threw error`);
    assert(errMsg.includes('trusted shell declaration in bro.json'),
           `error message for bro.${ns} mentions trusted shell requirement, got: "${errMsg}"`);
}

// Standard APIs remain available to untrusted apps
assert(bro.apps.available === true, 'bro.apps is available to untrusted app');
assert(bro.vfs.available === true, 'bro.vfs is available to untrusted app');
assert(bro.keys.available === true, 'bro.keys is available to untrusted app');
assert(bro.themes.available === true, 'bro.themes is available to untrusted app');
assert(bro.search.available === true, 'bro.search is available to untrusted app');

console.log('test_trust.js PASSED');

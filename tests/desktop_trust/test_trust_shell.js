// Test that an app requesting shell privileges receives them when trusted
// and is denied them when not trusted.

const privilegedNamespaces = ['displays', 'cred', 'seat', 'portal', 'sys', 'compositor', 'wl'];
const isTrusted = typeof bro.appDir === 'string' && bro.appDir.includes('trusted_app');

for (const ns of privilegedNamespaces) {
    assert(typeof bro[ns] === 'object', `bro.${ns} object exists`);
    if (isTrusted) {
        assert(bro[ns].available === true, `bro.${ns}.available is true for trusted shell app`);
    } else {
        assert(bro[ns].available === false, `bro.${ns}.available is false for untrusted app`);
    }
}

if (isTrusted) {
    console.log('test_trust_shell.js trusted shell check PASSED');
} else {
    console.log('test_trust_shell.js untrusted check PASSED');
}


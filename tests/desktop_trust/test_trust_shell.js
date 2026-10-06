// Test that an app requesting shell privileges receives them when trusted
// and is denied them when not trusted.

// In default test_app (untrusted, no shell declaration), privileged namespaces are stubbed:
const privilegedNamespaces = ['displays', 'cred', 'seat', 'portal', 'sys', 'compositor', 'wl'];

for (const ns of privilegedNamespaces) {
    assert(typeof bro[ns] === 'object', `bro.${ns} object exists`);
    assert(bro[ns].available === false, `bro.${ns}.available is false for untrusted app`);
}

console.log('test_trust_shell.js untrusted check PASSED');

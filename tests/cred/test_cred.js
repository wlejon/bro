// Headless test for bro.cred
assert(typeof bro.cred === 'object', 'bro.cred namespace exists');
if (!bro.cred.available) {
    skipTest('bro.cred is unavailable in this environment');
} else {
    // 1. Biometrics capabilities
    const bio = bro.cred.getBiometrics();
    assert(typeof bio === 'object' && bio !== null, 'getBiometrics returns object');
    assert(typeof bio.hasFingerprint === 'boolean', 'hasFingerprint is boolean');
    assert(typeof bio.hasFace === 'boolean', 'hasFace is boolean');

    // 2. Authentication
    const authSync = bro.cred.authenticateSync('test_user_xyz', 'invalid_password');
    assert(typeof authSync === 'boolean', 'authenticateSync returns boolean');

    const authPromise = bro.cred.authenticate('test_user_xyz', 'invalid_password');
    assert(typeof authPromise === 'object' && typeof authPromise.then === 'function', 'authenticate returns Promise');
    authPromise.catch(() => {});

    // 3. Secrets storage
    // Writes go to the account's real keystore (Credential Manager, Secret
    // Service, Keychain), so only a read of an entry that cannot exist is made;
    // brocred's own tests cover writes against an isolated store.
    assert(typeof bro.cred.setSecret === 'function', 'setSecret is function');
    const getPromise = bro.cred.getSecret('bro-headless-test-never-stored', 'nobody');
    assert(typeof getPromise === 'object' && typeof getPromise.then === 'function', 'getSecret returns Promise');
    getPromise.catch(() => {});

    // 4. Polkit registration
    assert(typeof bro.cred.registerPolkitAgent === 'function', 'registerPolkitAgent is function');
    assert(typeof bro.cred.unregisterPolkitAgent === 'function', 'unregisterPolkitAgent is function');

    console.log('test_cred.js PASSED');
}

// Headless test for bro.cred
assert(typeof bro.cred === 'object', 'bro.cred namespace exists');
assert(bro.cred.available === true, 'bro.cred.available is true for trusted shell app');

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
const setPromise = bro.cred.setSecret('test_service', 'test_acc', 'test_secret_123');
assert(typeof setPromise === 'object' && typeof setPromise.then === 'function', 'setSecret returns Promise');

const getPromise = bro.cred.getSecret('test_service', 'test_acc');
assert(typeof getPromise === 'object' && typeof getPromise.then === 'function', 'getSecret returns Promise');

// 4. Polkit registration
assert(typeof bro.cred.registerPolkitAgent === 'function', 'registerPolkitAgent is function');
assert(typeof bro.cred.unregisterPolkitAgent === 'function', 'unregisterPolkitAgent is function');

console.log('test_cred.js PASSED');

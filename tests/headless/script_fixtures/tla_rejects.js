// Fixture for test_script_rejection_fails.js: a top-level await that rejects.
const v = await Promise.resolve(1);
await Promise.reject(new Error('fixture: tla rejected ' + v));
console.log('FIXTURE_UNREACHABLE');

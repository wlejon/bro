/**
 * @file docs/cred-api.js
 * @summary Documentation and examples for the `bro.cred` JavaScript API.
 */

/**
 * `bro.cred` provides authentication, biometrics, secret storage, and PolicyKit integration:
 * - PAM / OS user authentication (`authenticate`, `authenticateSync`)
 * - Biometric capabilities and fingerprint/face authentication (`getBiometrics`, `startBiometricAuth`, `cancelBiometricAuth`)
 * - Secret service / credential storage (`getSecret`, `setSecret`, `deleteSecret`, `listSecrets`)
 * - PolicyKit authentication agent (`registerPolkitAgent`, `unregisterPolkitAgent`, `onPolkitRequest`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_CRED` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["cred", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. User Authentication (PAM)
// ============================================================================

if (bro.cred.available) {
    // Authenticate user password asynchronously (returns Promise<boolean>)
    bro.cred.authenticate('username', 'secretpassword').then(valid => {
        if (valid) {
            console.log('Authentication successful');
        } else {
            console.log('Authentication failed');
        }
    });

    // Synchronous authentication check
    const isValid = bro.cred.authenticateSync('username', 'secretpassword');
}

// ============================================================================
// 2. Biometric Authentication
// ============================================================================

const bioCaps = bro.cred.getBiometrics();
console.log('Biometric hardware available:', {
    hasFingerprint: bioCaps.hasFingerprint,
    hasFace: bioCaps.hasFace,
    hasVoice: bioCaps.hasVoice
});

if (bioCaps.hasFingerprint || bioCaps.hasFace) {
    // Start biometric authentication prompt
    bro.cred.startBiometricAuth().then(result => {
        console.log(`Biometric auth result: ${result.success}, method: ${result.method}`);
    }).catch(err => {
        console.error('Biometric auth error:', err.message);
    });

    // To cancel pending biometric auth:
    // bro.cred.cancelBiometricAuth();
}

// ============================================================================
// 3. Secret Storage
// ============================================================================

// Store a secret credential
await bro.cred.setSecret('my_service', 'user@example.com', 'super_secret_token', {
    created: Date.now(),
    description: 'API access token'
});

// Retrieve secret credential
const secret = await bro.cred.getSecret('my_service', 'user@example.com');
console.log('Retrieved secret token:', secret ? '[present]' : '[not found]');

// List stored secrets
const secretsList = await bro.cred.listSecrets('my_service');
for (const entry of secretsList) {
    console.log(`Secret: ${entry.service} / ${entry.account}`);
}

// Delete secret
await bro.cred.deleteSecret('my_service', 'user@example.com');

// ============================================================================
// 4. PolicyKit Agent
// ============================================================================

// Register as active session PolicyKit agent
const agentRegistered = bro.cred.registerPolkitAgent();

if (agentRegistered) {
    // Listen for privilege elevation requests
    const polkitSub = bro.cred.onPolkitRequest((request) => {
        console.log(`Polkit auth request for action: ${request.actionId}, message: ${request.message}`);
        // Prompt user and submit response:
        // request.submitResponse('password');
        // or cancel:
        // request.cancel();
    });

    // Unregister when done
    // bro.cred.unregisterPolkitAgent();
}

/**
 * @file docs/cred-api.js
 * @summary Documentation and examples for the `bro.cred` JavaScript API.
 */

/**
 * `bro.cred` provides authentication, biometrics, secret storage, and PolicyKit integration:
 * - PAM / OS user authentication (`authenticate`, `authenticateSync`)
 * - Biometric capabilities and fingerprint/face/iris authentication (`getBiometrics`, `startBiometricAuth`, `cancelBiometricAuth`)
 * - Secret service / credential storage (`getSecret`, `setSecret`, `deleteSecret`, `listSecrets`)
 * - PolicyKit authentication agent (`registerPolkitAgent`, `unregisterPolkitAgent`, `onPolkitRequest`, `submitResponse`, `cancelRequest`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_CRED` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"permissions": ["cred", ...]` in its `bro.json`.
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
// {
//   supported: boolean,
//   hasFingerprint: boolean,
//   hasFace: boolean,
//   hasIris: boolean,
//   availability: string,
//   primaryType: 'fingerprint' | 'face' | 'iris' | 'none' | 'unknown',
//   sensorName: string,
//   details: string
// }
console.log('Biometric hardware available:', {
    supported: bioCaps.supported,
    hasFingerprint: bioCaps.hasFingerprint,
    hasFace: bioCaps.hasFace,
    hasIris: bioCaps.hasIris,
    primaryType: bioCaps.primaryType,
    sensorName: bioCaps.sensorName
});

if (bioCaps.hasFingerprint || bioCaps.hasFace || bioCaps.hasIris) {
    // Start biometric authentication prompt
    bro.cred.startBiometricAuth().then(result => {
        // { success: boolean, method: string, availability?: string, details?: string, cancelled?: boolean, error?: string }
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

// Store a secret credential (sync variant: setSecretSync)
await bro.cred.setSecret('my_service', 'user@example.com', 'super_secret_token', {
    created: String(Date.now()),
    description: 'API access token'
});

// Retrieve secret credential (sync variant: getSecretSync)
const secret = await bro.cred.getSecret('my_service', 'user@example.com');
console.log('Retrieved secret token:', secret ? '[present]' : '[not found]');

// List stored secrets (sync variant: listSecretsSync)
// Returns Array<{ service, account, meta, attributes, lastModified }>
const secretsList = await bro.cred.listSecrets('my_service');
for (const entry of secretsList) {
    console.log(`Secret: ${entry.service} / ${entry.account} (modified: ${entry.lastModified})`);
}

// Delete secret (sync variant: deleteSecretSync)
await bro.cred.deleteSecret('my_service', 'user@example.com');

// ============================================================================
// 4. PolicyKit Agent
// ============================================================================

// Register as active session PolicyKit agent
// Options: { registerWithAuthority?: boolean, objectPath?: string, busAddress?: string, fallbackPassword?: string }
const agentRegistered = bro.cred.registerPolkitAgent({
    registerWithAuthority: true
});

if (agentRegistered) {
    // Listen for privilege elevation requests
    // Request shape:
    // {
    //   actionId, action_id, message, iconName, icon_name, cookie,
    //   details: Object,
    //   identities: [{ kind: 'user' | 'group', id: number, name: string }],
    //   submitResponse(response),
    //   cancel(reason?)
    // }
    const polkitSub = bro.cred.onPolkitRequest((request) => {
        console.log(`Polkit auth request for action: ${request.actionId}, message: ${request.message}`);
        // Authenticate and submit response via request helper or top-level method:
        // request.submitResponse({ authenticated: true, selectedUser: 'j', password: '...' });
        // or: bro.cred.submitResponse(request.cookie, true);
        // or cancel:
        // request.cancel('User dismissed dialog');
        // or: bro.cred.cancelRequest(request.cookie);
    });

    // Unsubscribe listener or unregister agent when done:
    // polkitSub.cancel(); // or polkitSub.dispose() / polkitSub.unregister()
    // bro.cred.unregisterPolkitAgent();
}

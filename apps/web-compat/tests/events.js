// Headless smoke tests for new web-compat features.
// Animation/transition events require the windowed render loop's style
// resolution, so they are tested interactively (see index.html).

// ---- Scroll events ----
var scrollBox = document.getElementById('scroll-box');
var scrollFired = false;
scrollBox.addEventListener('scroll', function() { scrollFired = true; });
scrollBox.scrollTop = 50;
flush();
assert(scrollFired, 'scroll event fires on programmatic scrollTop');

// ---- crypto.subtle ----
assert(typeof crypto.subtle === 'object', 'crypto.subtle exists');
assert(typeof crypto.subtle.digest === 'function', 'subtle.digest exists');
assert(typeof crypto.subtle.sign === 'function', 'subtle.sign exists');
assert(typeof crypto.subtle.verify === 'function', 'subtle.verify exists');
assert(typeof crypto.subtle.encrypt === 'function', 'subtle.encrypt exists');
assert(typeof crypto.subtle.decrypt === 'function', 'subtle.decrypt exists');
assert(typeof crypto.subtle.generateKey === 'function', 'subtle.generateKey exists');
assert(typeof crypto.subtle.importKey === 'function', 'subtle.importKey exists');
assert(typeof crypto.subtle.exportKey === 'function', 'subtle.exportKey exists');

async function testCrypto() {
    // digest
    var hash = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('hello'));
    assert(hash.byteLength === 32, 'SHA-256 digest produces 32 bytes');

    // HMAC sign + verify
    var key = await crypto.subtle.generateKey(
        { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']
    );
    var sig = await crypto.subtle.sign({ name: 'HMAC' }, key, new TextEncoder().encode('test'));
    assert(sig.byteLength === 32, 'HMAC-SHA256 signature is 32 bytes');

    var valid = await crypto.subtle.verify(
        { name: 'HMAC' }, key, sig, new TextEncoder().encode('test')
    );
    assert(valid === true, 'HMAC verify succeeds');

    var invalid = await crypto.subtle.verify(
        { name: 'HMAC' }, key, sig, new TextEncoder().encode('tampered')
    );
    assert(invalid === false, 'HMAC verify rejects tampered data');

    // AES-GCM roundtrip
    var aesKey = await crypto.subtle.generateKey(
        { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']
    );
    var iv = new Uint8Array(12);
    crypto.getRandomValues(iv);
    var pt = new TextEncoder().encode('secret');
    var ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv }, aesKey, pt);
    assert(ct.byteLength > pt.byteLength, 'ciphertext includes auth tag');
    var dec = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: iv }, aesKey, ct);
    assert(new TextDecoder().decode(dec) === 'secret', 'AES-GCM roundtrip recovers plaintext');

    console.log('All web-compat headless tests passed!');
}

testCrypto();

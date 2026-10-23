#include "net/rollback_session.h"
#include "net/bronze_api/net_rollback_api.h"

#include "embed/embed.h"
#include "eval/eval.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace ev = bronze::embed;

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        std::exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); \
    auto _b = (b); \
    if (_a != _b) { \
        std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        std::exit(1); \
    } \
} while (0)

namespace {

// Deterministic integer 2-player physics step used by C++ tests
void deterministicPhysicsTick(std::vector<uint8_t>& stateBytes,
                              std::span<const int32_t> inputs,
                              uint32_t frame) {
    if (stateBytes.size() < 4 * sizeof(int32_t)) return;
    int32_t st[4] = {0, 0, 0, 0};
    std::memcpy(st, stateBytes.data(), sizeof(st));

    const int32_t in0 = !inputs.empty() ? inputs[0] : 0;
    const int32_t in1 = (inputs.size() >= 2) ? inputs[1] : 0;

    // st[0] = p0_x, st[1] = p0_vx, st[2] = p1_x, st[3] = p1_vx
    st[1] = st[1] + in0;
    st[0] = st[0] + st[1] + static_cast<int32_t>(frame & 3u);
    st[3] = st[3] + in1;
    st[2] = st[2] + st[3] - static_cast<int32_t>(frame & 1u);

    std::memcpy(stateBytes.data(), st, sizeof(st));
}

void test_cpp_determinism_gate() {
    std::printf("[1/4] C++ determinism proof gate...\n");

    const char* cleanTick = R"JS(
        function tick(state, inputs, frame) {
            state[0] = (state[0] + inputs[0] + (frame | 0)) | 0;
            state[1] = (state[1] + inputs[1] * 2) | 0;
        }
    )JS";
    auto cleanAudit = bro::net::verifyRollbackTickSource(cleanTick);
    ASSERT_TRUE(cleanAudit.deterministic);
    ASSERT_EQ(cleanAudit.certificateHash.size(), 64u);
    ASSERT_TRUE(cleanAudit.hazards.empty());

    // Math.random() must be rejected as unseeded-prng
    auto rngAudit = bro::net::verifyRollbackTickSource(
        "(state, inputs) => { state[0] += Math.random() > 0.5 ? 1 : 0; }");
    ASSERT_TRUE(!rngAudit.deterministic);
    ASSERT_TRUE(!rngAudit.hazards.empty());
    ASSERT_EQ(rngAudit.hazards[0].kind, std::string("unseeded-prng"));

    // Date.now() must be rejected as wall-clock-time
    auto timeAudit = bro::net::verifyRollbackTickSource(
        "function(state, inputs) { state[0] = Date.now() | 0; }");
    ASSERT_TRUE(!timeAudit.deterministic);
    ASSERT_TRUE(!timeAudit.hazards.empty());
    ASSERT_EQ(timeAudit.hazards[0].kind, std::string("wall-clock-time"));

    // performance.now() must be rejected as wall-clock-time
    auto perfAudit = bro::net::verifyRollbackTickSource(
        "(state) => { state[0] = performance.now() | 0; }");
    ASSERT_TRUE(!perfAudit.deterministic);
    ASSERT_TRUE(!perfAudit.hazards.empty());
    ASSERT_EQ(perfAudit.hazards[0].kind, std::string("wall-clock-time"));
}

void test_cpp_two_peer_rollback_convergence() {
    std::printf("[2/4] C++ 2-peer 10-frame prediction, 6-frame rollback & FNV-1a convergence...\n");

    const std::string cleanTickSrc = R"JS(
        (state, inputs, frame) => {
            state[1] = (state[1] + inputs[0]) | 0;
            state[0] = (state[0] + state[1] + (frame & 3)) | 0;
            state[3] = (state[3] + inputs[1]) | 0;
            state[2] = (state[2] + state[3] - (frame & 1)) | 0;
        }
    )JS";

    bro::net::RollbackConfig cfgA;
    cfgA.playerCount = 2;
    cfgA.localPlayerId = 0;
    cfgA.maxRollbackFrames = 64;
    cfgA.stateSize = 16;
    cfgA.tickSource = cleanTickSrc;

    bro::net::RollbackConfig cfgB = cfgA;
    cfgB.localPlayerId = 1;

    bro::net::RollbackSession peerA(cfgA, deterministicPhysicsTick);
    bro::net::RollbackSession peerB(cfgB, deterministicPhysicsTick);

    ASSERT_TRUE(peerA.isDeterministic());
    ASSERT_TRUE(peerB.isDeterministic());
    ASSERT_EQ(peerA.certificateHash(), peerB.certificateHash());

    // Frames 0..3: Player 0 inputs +2, Player 1 inputs +3 (both peers confirm frames 0..3)
    for (uint32_t f = 0; f < 4; ++f) {
        peerA.receiveRemoteInput(f, 1, 3);
        peerB.receiveRemoteInput(f, 0, 2);
        peerA.advanceFrameSingle(2);
        peerB.advanceFrameSingle(3);
    }
    ASSERT_EQ(peerA.currentFrame(), 4u);
    ASSERT_EQ(peerB.currentFrame(), 4u);
    ASSERT_EQ(peerA.currentChecksumHex(), peerB.currentChecksumHex());

    // Frames 4..9 (6 frames):
    // Player 0 continues inputting +2 (matching Peer B's prediction of Player 0!).
    // Player 1 changes input to -5 at frame 4 and holds -5 for frames 4..9!
    // Peer A does NOT receive Player 1's packet yet, so Peer A predicts +3 for frames 4..9.
    for (uint32_t f = 4; f < 10; ++f) {
        peerA.advanceFrameSingle(2);
        peerB.advanceFrameSingle(-5);
    }

    ASSERT_EQ(peerA.currentFrame(), 10u);
    ASSERT_EQ(peerB.currentFrame(), 10u);
    // Before delayed input arrives, Peer A and Peer B have diverged at frame 10:
    ASSERT_TRUE(peerA.currentChecksumHex() != peerB.currentChecksumHex());

    // Now Peer A receives Player 1's delayed input (-5) from 6 frames ago (frame 4)!
    auto rbRes = peerA.receiveRemoteInput(4u, 1u, -5, peerB.getSnapshot(5u)->checksumHex);
    ASSERT_TRUE(rbRes.rolledBack);
    ASSERT_EQ(rbRes.framesResimulated, 6u);
    ASSERT_EQ(rbRes.currentFrame, 10u);
    ASSERT_TRUE(!rbRes.desyncDetected);
    ASSERT_EQ(peerA.rollbackCount(), 1u);
    ASSERT_EQ(peerA.totalFramesResimulated(), 6u);

    // Both peers must now have bit-identical 64-bit FNV-1a checksums across all frames 0..10!
    ASSERT_EQ(peerA.currentChecksumHex(), peerB.currentChecksumHex());
    for (uint32_t f = 0; f <= 10; ++f) {
        const auto* snapB = peerB.getSnapshot(f);
        ASSERT_TRUE(snapB != nullptr);
        ASSERT_TRUE(peerA.verifyPeerChecksum(f, snapB->checksumHex));
    }
    ASSERT_TRUE(!peerA.desyncDetected());

    // Verify deterministic binary replay export & verification
    std::vector<uint8_t> replay = peerA.exportReplay();
    auto verifyRes = peerB.verifyReplay(replay);
    ASSERT_TRUE(verifyRes.ok);
    ASSERT_EQ(verifyRes.framesSimulated, 10u);
    ASSERT_EQ(verifyRes.actualChecksum, peerB.currentChecksumHex());
}

void test_cpp_tampering_detection() {
    std::printf("[3/4] C++ intentional state tampering detection...\n");

    bro::net::RollbackConfig cfg;
    cfg.playerCount = 2;
    cfg.localPlayerId = 0;
    cfg.stateSize = 16;
    cfg.tickSource = "(state, inputs) => { state[0] = (state[0] + inputs[0]) | 0; }";

    bro::net::RollbackSession peerA(cfg, deterministicPhysicsTick);
    bro::net::RollbackSession peerB(cfg, deterministicPhysicsTick);

    peerA.advanceFrameSingle(4);
    peerB.advanceFrameSingle(4);
    ASSERT_TRUE(peerA.verifyPeerChecksum(1u, peerB.currentChecksumHex()));
    ASSERT_TRUE(!peerA.desyncDetected());

    // Intentionally corrupt Peer A's state bytes
    std::vector<uint8_t> corrupted = peerA.stateBytes();
    corrupted[0] ^= 0xFFu;
    peerA.setStateBytes(corrupted);

    ASSERT_TRUE(!peerA.verifyPeerChecksum(1u, peerB.currentChecksumHex()));
    ASSERT_TRUE(peerA.desyncDetected());
}

void test_js_rollback_session_api() {
    std::printf("[4/4] JS bro.net.createRollbackSession end-to-end test...\n");

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        bro::bronze_host::installNetRollbackExtensions();

        ev::CallResult res = bronze::eval::evalScript(R"JS(
            (() => {
                if (!bro || !bro.net || typeof bro.net.createRollbackSession !== "function") {
                    throw new Error("missing bro.net.createRollbackSession");
                }

                // 1. Reject non-deterministic tick functions calling Math.random() or Date.now()
                let rejectedRandom = false;
                try {
                    bro.net.createRollbackSession({
                        tick: (state, inputs) => {
                            state[0] += Math.random() > 0.5 ? 1 : 0;
                        }
                    });
                } catch (e) {
                    rejectedRandom = String(e).includes("unseeded-prng");
                }
                if (!rejectedRandom) {
                    throw new Error("Expected Math.random() tick to be rejected with unseeded-prng");
                }

                let rejectedDate = false;
                try {
                    bro.net.createRollbackSession({
                        tick: (state, inputs) => {
                            state[0] = Date.now() | 0;
                        }
                    });
                } catch (e) {
                    rejectedDate = String(e).includes("wall-clock-time");
                }
                if (!rejectedDate) {
                    throw new Error("Expected Date.now() tick to be rejected with wall-clock-time");
                }

                // 2. Create two verified deterministic rollback sessions for Peer 0 and Peer 1
                const deterministicTick = (state, inputs, frame) => {
                    state[1] = (state[1] + inputs[0]) | 0;
                    state[0] = (state[0] + state[1] + (frame & 3)) | 0;
                    state[3] = (state[3] + inputs[1]) | 0;
                    state[2] = (state[2] + state[3] - (frame & 1)) | 0;
                };

                const peer0 = bro.net.createRollbackSession({
                    tick: deterministicTick,
                    initialState: new Int32Array([100, 0, 200, 0]),
                    playerCount: 2,
                    localPlayerId: 0,
                    maxRollbackFrames: 64
                });

                const peer1 = bro.net.createRollbackSession({
                    tick: deterministicTick,
                    initialState: new Int32Array([100, 0, 200, 0]),
                    playerCount: 2,
                    localPlayerId: 1,
                    maxRollbackFrames: 64
                });

                if (!peer0.deterministic || !peer1.deterministic) {
                    throw new Error("Expected deterministic === true");
                }
                if (typeof peer0.certificateHash !== "string" || peer0.certificateHash.length !== 64) {
                    throw new Error("Expected 64-char SHA-256 certificateHash, got: " + peer0.certificateHash);
                }
                if (peer0.certificateHash !== peer1.certificateHash) {
                    throw new Error("Certificate hash mismatch between peers");
                }

                // Advance frames 0..3 with confirmed inputs (p0 = 1, p1 = 2)
                for (let f = 0; f < 4; f++) {
                    peer0.receiveRemoteInput(f, 1, 2);
                    peer1.receiveRemoteInput(f, 0, 1);
                    peer0.advanceFrame(1);
                    peer1.advanceFrame(2);
                }

                // Advance frames 4..9 (6 frames):
                // Player 0 keeps input 1; Player 1 switches to -4 at frame 4.
                // Peer 0 hasn't received Player 1's input for frames 4..9 yet, so it predicts +2.
                for (let f = 4; f < 10; f++) {
                    peer0.advanceFrame(1);
                    peer1.advanceFrame(-4);
                }

                if (peer0.currentFrame !== 10 || peer1.currentFrame !== 10) {
                    throw new Error("Expected currentFrame === 10");
                }
                if (peer0.checksumHex === peer1.checksumHex) {
                    throw new Error("Expected divergent checksums before delayed remote input arrives");
                }

                // Deliver delayed remote input for frame 4 (6 frames ago) to Peer 0
                const rb = peer0.receiveRemoteInput(4, 1, -4, peer1.getSnapshot(5).checksumHex);
                if (!rb.rolledBack || rb.framesResimulated !== 6 || rb.desyncDetected) {
                    throw new Error("Unexpected rollback result: " + JSON.stringify(rb));
                }

                // Both peers must converge to bit-identical 64-bit FNV-1a checksums at frame 10!
                const snap10 = peer1.getSnapshot(10);
                if (!peer0.verifyPeerChecksum(10, snap10.checksumHex)) {
                    throw new Error("Peer 0 and Peer 1 checksums did not converge after rollback!");
                }
                if (peer0.desyncDetected || peer0.getStats().rollbackCount !== 1) {
                    throw new Error("Unexpected stats after convergence: " + JSON.stringify(peer0.getStats()));
                }

                // 3. Replay export & verification
                const replayBytes = peer0.exportReplay();
                const replayCheck = peer1.verifyReplay(replayBytes);
                if (!replayCheck.ok || replayCheck.framesSimulated !== 10) {
                    throw new Error("Replay verification failed: " + JSON.stringify(replayCheck));
                }

                // 4. Intentional state tampering detection
                peer0.state[0] = 999999;
                if (peer0.verifyPeerChecksum(10, snap10.checksumHex) !== false) {
                    throw new Error("Tampered state should fail verifyPeerChecksum");
                }
                if (!peer0.desyncDetected || !peer0.getStats().desyncDetected) {
                    throw new Error("Tampered state should set desyncDetected = true");
                }

                return "SUCCESS";
            })()
        )JS");

        if (res.thrown) {
            std::fprintf(stderr, "JS eval threw: %s\n", ev::toUtf8(res.value).c_str());
            std::exit(1);
        }
        ASSERT_EQ(ev::toUtf8(res.value), std::string("SUCCESS"));
    }
    ev::destroyRealm(realm);
}

void test_cpp_and_js_verified_capsules() {
    std::printf("[5/5] C++ & JS Proof-Carrying P2P Code Capsules (Issue #151)...\n");

    // 1. C++ verified capsule creation & verification
    const char* safeMod = R"JS(
        (state, inputs, frame) => {
            state[0] = (state[0] + inputs[0] * 3 + (frame & 7)) | 0;
            return state[0];
        }
    )JS";
    auto cap = bro::net::createVerifiedCapsule(safeMod, "stepMod", "peer-alpha");
    ASSERT_TRUE(cap.deterministic);
    ASSERT_TRUE(cap.hazardFree);
    ASSERT_EQ(cap.hazardCount, 0u);
    ASSERT_EQ(cap.sha256.size(), 64u);
    ASSERT_EQ(cap.certificateHash.size(), 64u);

    auto verOk = bro::net::verifyCapsule(cap);
    ASSERT_TRUE(verOk.valid);
    ASSERT_TRUE(!verOk.tampered);
    ASSERT_EQ(verOk.reason, std::string("verified"));

    // 2. Tampered source in transit must set tampered = true and valid = false
    auto tamperedSrc = cap;
    tamperedSrc.source += "\n// injected";
    auto verTamperedSrc = bro::net::verifyCapsule(tamperedSrc);
    ASSERT_TRUE(!verTamperedSrc.valid);
    ASSERT_TRUE(verTamperedSrc.tampered);

    // 3. Forged certificateHash must set tampered = true and valid = false
    auto forgedCert = cap;
    forgedCert.certificateHash[0] = (forgedCert.certificateHash[0] == 'a') ? 'b' : 'a';
    auto verForged = bro::net::verifyCapsule(forgedCert);
    ASSERT_TRUE(!verForged.valid);
    ASSERT_TRUE(verForged.tampered);

    // 4. Sandbox escape (`eval`) and division-by-zero (`/ 0`) detection
    auto evalCap = bro::net::createVerifiedCapsule("(x) => eval('x + 1')", "badEval", "peer-mal");
    ASSERT_TRUE(!evalCap.deterministic);
    ASSERT_TRUE(!evalCap.hazardFree);
    ASSERT_TRUE(!bro::net::verifyCapsule(evalCap).valid);

    auto divZeroCap = bro::net::createVerifiedCapsule("(x) => (x / 0) | 0", "badDiv", "peer-mal");
    ASSERT_TRUE(!divZeroCap.deterministic);
    ASSERT_TRUE(!divZeroCap.hazardFree);
    ASSERT_TRUE(!bro::net::verifyCapsule(divZeroCap).valid);

    // 5. JS API: bro.net.createVerifiedCapsule, bro.net.verifyCapsule, bro.net.loadVerifiedCapsule
    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        bro::bronze_host::installNetRollbackExtensions();

        ev::CallResult res = bronze::eval::evalScript(R"JS(
            (() => {
                if (typeof bro.net.createVerifiedCapsule !== "function" ||
                    typeof bro.net.verifyCapsule !== "function" ||
                    typeof bro.net.loadVerifiedCapsule !== "function") {
                    throw new Error("missing VerifiedCapsule functions on bro.net");
                }

                // Package a deterministic P2P physics modifier function
                const modFn = (state, bonus, frame) => {
                    state[0] = (state[0] + bonus + (frame & 3)) | 0;
                    state[1] = (state[1] ^ bonus) | 0;
                    return state[0];
                };

                const capsule = bro.net.createVerifiedCapsule(modFn, {
                    entryName: "applyBonus",
                    authorId: "peer-1"
                });

                if (!capsule.deterministic || !capsule.hazardFree || capsule.hazardCount !== 0) {
                    throw new Error("Expected clean capsule to be deterministic and hazardFree");
                }
                if (capsule.sha256.length !== 64 || capsule.certificateHash.length !== 64) {
                    throw new Error("Expected 64-hex sha256 and certificateHash");
                }

                // Simulate P2P transmission over JSON wire payload
                const wireJson = JSON.stringify(capsule);
                const verifyRes = bro.net.verifyCapsule(wireJson, {
                    expectedSha256: capsule.sha256,
                    expectedCertificateHash: capsule.certificateHash
                });
                if (!verifyRes.valid || verifyRes.tampered) {
                    throw new Error("Expected wireJson capsule to verify: " + JSON.stringify(verifyRes));
                }

                // Load and execute the verified capsule on the receiving peer
                const loaded = bro.net.loadVerifiedCapsule(wireJson);
                if (!loaded.valid || typeof loaded.fn !== "function") {
                    throw new Error("Expected loadVerifiedCapsule to return callable fn");
                }
                const st = new Int32Array([10, 5]);
                const out0 = loaded.fn(st, 7, 2);
                if (out0 !== 19 || st[0] !== 19 || st[1] !== (5 ^ 7)) {
                    throw new Error("Unexpected loaded capsule output: " + out0);
                }

                // Tamper with the capsule source while keeping the old sha256/certificateHash
                const tampered = JSON.parse(wireJson);
                tampered.source = "(state, bonus, frame) => { state[0] = 999999; return 999999; }";
                const tamperedCheck = bro.net.verifyCapsule(tampered);
                if (tamperedCheck.valid !== false || tamperedCheck.tampered !== true) {
                    throw new Error("Expected tampered capsule to fail with tampered: true");
                }

                let threwOnTamperedLoad = false;
                try {
                    bro.net.loadVerifiedCapsule(tampered);
                } catch (e) {
                    threwOnTamperedLoad = String(e).includes("mismatch") || String(e).includes("tampered");
                }
                if (!threwOnTamperedLoad) {
                    throw new Error("Expected loadVerifiedCapsule to throw on tampered capsule");
                }

                // Hazardous capsule (Math.random) must be rejected by verifyCapsule & loadVerifiedCapsule
                const badCapsule = bro.net.createVerifiedCapsule(
                    (state) => { state[0] = Math.random() > 0.5 ? 1 : 0; },
                    { entryName: "badMod", authorId: "peer-2" }
                );
                if (badCapsule.deterministic !== false || badCapsule.hazardCount < 1) {
                    throw new Error("Expected Math.random capsule to be flagged non-deterministic");
                }
                const badVerify = bro.net.verifyCapsule(badCapsule);
                if (badVerify.valid !== false) {
                    throw new Error("Expected verifyCapsule to reject hazardous capsule");
                }

                return "CAPSULE_OK";
            })()
        )JS");

        if (res.thrown) {
            std::fprintf(stderr, "JS capsule test threw: %s\n", ev::toUtf8(res.value).c_str());
            std::exit(1);
        }
        ASSERT_EQ(ev::toUtf8(res.value), std::string("CAPSULE_OK"));
    }
    ev::destroyRealm(realm);
}

}  // namespace

int main() {
    test_cpp_determinism_gate();
    test_cpp_two_peer_rollback_convergence();
    test_cpp_tampering_detection();
    test_js_rollback_session_api();
    test_cpp_and_js_verified_capsules();
    std::printf("All rollback netcode & verified capsule tests passed!\n");
    return 0;
}

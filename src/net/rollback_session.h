#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bro::net {

struct DeterminismHazard {
    std::string kind;
    std::string message;
    std::string remediation;
    std::string functionName;
    uint32_t line = 1;
    uint32_t column = 1;
};

struct DeterminismAuditResult {
    bool determinismLintPassed = false;
    std::string certificateHash;
    std::vector<DeterminismHazard> hazards;
    std::string errorSummary;
};

// Audits a JS tick function's source code using bronze's static determinism lint
// (rejecting Date/performance/Math.random/transcendental Math/for..in/host I/O
// hazards and checking IL + x86_64/AArch64 LIR parity) and computes a SHA-256
// hex source audit digest.
DeterminismAuditResult verifyRollbackTickSource(std::string_view tickSource);

struct VerifiedCapsule {
    uint32_t version = 1;
    std::string source;
    std::string entryName = "capsule";
    std::string authorId = "peer-0";
    bool determinismLintPassed = false;
    bool hazardFree = false;
    uint32_t hazardCount = 0;
    std::vector<DeterminismHazard> hazards;
    std::string sha256;
    std::string certificateHash;
    std::string errorSummary;
};

struct CapsulePolicy {
    bool requireDeterminismLint = true;
    bool requireHazardFree = true;
    std::string expectedSha256;
    std::string expectedCertificateHash;
};

struct CapsuleVerifyResult {
    bool valid = false;
    bool tampered = false;
    bool determinismLintPassed = false;
    bool hazardFree = false;
    uint32_t hazardCount = 0;
    std::vector<DeterminismHazard> hazards;
    std::string sha256;
    std::string certificateHash;
    std::string reason;
};

// Packages a JS function/module source into a Proof-Carrying P2P Code Capsule
// with a SHA-256 source digest, deterministic lockstep + sandbox safety audit,
// and cryptographic certificate hash (Issue #151).
VerifiedCapsule createVerifiedCapsule(std::string_view source,
                                      std::string_view entryName = "capsule",
                                      std::string_view authorId = "peer-0");

// Re-audits a received P2P Code Capsule from scratch, detects tampering or
// falsified proof metadata, and enforces the receiver's CapsulePolicy (Issue #151).
CapsuleVerifyResult verifyCapsule(const VerifiedCapsule& capsule,
                                  const CapsulePolicy& policy = {});

// Computes the canonical 64-bit FNV-1a checksum of a state byte buffer.
uint64_t computeStateFnv1a64(std::span<const uint8_t> bytes) noexcept;

// Formats a 64-bit checksum as a 16-character lowercase hex string.
std::string formatChecksumHex(uint64_t checksum);

inline uint32_t checksumLow32(uint64_t checksum) noexcept {
    return static_cast<uint32_t>(checksum & 0xFFFFFFFFu);
}

struct RollbackConfig {
    uint32_t playerCount = 2;
    uint32_t localPlayerId = 0;
    uint32_t maxRollbackFrames = 64;
    uint32_t stateSize = 64;
    bool allowUnverified = false;
    std::string tickSource;
    std::vector<uint8_t> initialStateBytes;
};

struct RollbackSnapshot {
    uint32_t frame = 0;
    std::vector<uint8_t> stateBytes;
    std::vector<int32_t> inputs;
    std::vector<uint8_t> inputConfirmed;
    uint64_t checksum = 0;
    std::string checksumHex;
    bool valid = false;
};

struct RollbackAdvanceResult {
    uint32_t frame = 0;
    uint64_t checksum = 0;
    std::string checksumHex;
    uint32_t checksumLow32 = 0;
};

struct RollbackRemoteInputResult {
    bool rolledBack = false;
    uint32_t framesResimulated = 0;
    uint32_t currentFrame = 0;
    std::string currentChecksum;
    bool desyncDetected = false;
};

struct RollbackStats {
    uint32_t currentFrame = 0;
    uint32_t rollbackCount = 0;
    uint64_t totalFramesResimulated = 0;
    bool determinismLintPassed = false;
    bool desyncDetectionEnabled = true;
    std::string certificateHash;
    bool desyncDetected = false;
};

struct ReplayVerifyResult {
    bool ok = false;
    uint32_t framesSimulated = 0;
    std::string expectedChecksum;
    std::string actualChecksum;
    bool determinismLintPassed = false;
};

class RollbackSession {
public:
    using TickFn = std::function<void(std::vector<uint8_t>& stateBytes,
                                      std::span<const int32_t> inputs,
                                      uint32_t frame)>;

    RollbackSession(const RollbackConfig& config, TickFn tickFn);

    bool determinismLintPassed() const noexcept { return audit_.determinismLintPassed; }
    bool desyncDetectionEnabled() const noexcept { return true; }
    const std::string& certificateHash() const noexcept { return audit_.certificateHash; }
    const std::vector<DeterminismHazard>& hazards() const noexcept { return audit_.hazards; }
    const std::string& errorSummary() const noexcept { return audit_.errorSummary; }

    uint32_t currentFrame() const noexcept { return currentFrame_; }
    uint32_t playerCount() const noexcept { return playerCount_; }
    uint32_t localPlayerId() const noexcept { return localPlayerId_; }
    uint32_t maxRollbackFrames() const noexcept { return maxRollbackFrames_; }
    uint32_t rollbackCount() const noexcept { return rollbackCount_; }
    uint64_t totalFramesResimulated() const noexcept { return totalFramesResimulated_; }
    bool desyncDetected() const noexcept { return desyncDetected_; }

    const std::vector<uint8_t>& stateBytes() const noexcept { return currentStateBytes_; }
    void setStateBytes(std::span<const uint8_t> bytes);

    uint64_t currentChecksum() const noexcept { return currentChecksum_; }
    const std::string& currentChecksumHex() const noexcept { return currentChecksumHex_; }

    // Advance one frame:
    // - `localInputs` can provide either 1 input (for `localPlayerId`) or up to
    //   `playerCount` inputs; any player not in `confirmedMask` whose input for
    //   `currentFrame` was not already confirmed via `receiveRemoteInput` uses
    //   input prediction (repeating their last confirmed input, or 0).
    RollbackAdvanceResult advanceFrame(std::span<const int32_t> providedInputs,
                                       uint32_t providedMask = 0);

    // Convenience overload for single local player input.
    RollbackAdvanceResult advanceFrameSingle(int32_t localInput);

    // Record a confirmed remote (or local) player input for `frame`.
    // If `frame < currentFrame` and `actualInput` differs from the input used
    // when `frame` was simulated, rolls back to `frame` and re-simulates
    // `frame .. currentFrame - 1`.
    RollbackRemoteInputResult receiveRemoteInput(uint32_t frame,
                                                 uint32_t playerId,
                                                 int32_t actualInput,
                                                 std::string_view remoteChecksumHex = {});

    bool verifyPeerChecksum(uint32_t frame, std::string_view remoteChecksumHex);

    const RollbackSnapshot* getSnapshot(uint32_t frame) const noexcept;
    RollbackStats getStats() const;

    // Deterministic binary replay export & verification.
    std::vector<uint8_t> exportReplay() const;
    ReplayVerifyResult verifyReplay(std::span<const uint8_t> replayBytes) const;

private:
    RollbackSnapshot& slotFor(uint32_t frame) noexcept {
        return ring_[frame % ring_.size()];
    }
    const RollbackSnapshot& slotFor(uint32_t frame) const noexcept {
        return ring_[frame % ring_.size()];
    }

    int32_t predictedInputFor(uint32_t frame, uint32_t playerId) const noexcept;
    void saveStateSnapshot(uint32_t frame, const std::vector<uint8_t>& bytes);

    uint32_t playerCount_ = 2;
    uint32_t localPlayerId_ = 0;
    uint32_t maxRollbackFrames_ = 64;
    DeterminismAuditResult audit_;
    TickFn tickFn_;

    uint32_t currentFrame_ = 0;
    uint32_t rollbackCount_ = 0;
    uint64_t totalFramesResimulated_ = 0;
    bool desyncDetected_ = false;

    std::vector<uint8_t> initialStateBytes_;
    std::vector<uint8_t> currentStateBytes_;
    uint64_t currentChecksum_ = 0;
    std::string currentChecksumHex_;

    std::vector<RollbackSnapshot> ring_;
    // Full input log from frame 0 for deterministic replay export and prediction lookup.
    struct FrameInputRecord {
        std::vector<int32_t> inputs;
        std::vector<uint8_t> confirmed;
    };
    std::vector<FrameInputRecord> inputHistory_;
    std::vector<int32_t> lastConfirmedInput_;
    std::vector<bool> hasConfirmedInput_;
};

}  // namespace bro::net

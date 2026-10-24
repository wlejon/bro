#include "net/rollback_session.h"

#include "check/deterministic_check.h"
#include "util/sha.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace bro::net {

namespace {

std::string computeSha256Hex(std::string_view input) {
    bro::util::Sha256 h;
    h.update(input.data(), input.size());
    return h.hex();
}

void offsetToLineCol(std::string_view src, uint32_t offset, uint32_t& outLine, uint32_t& outCol) noexcept {
    outLine = 1;
    outCol = 1;
    const size_t limit = std::min<size_t>(offset, src.size());
    for (size_t i = 0; i < limit; ++i) {
        if (src[i] == '\n') {
            ++outLine;
            outCol = 1;
        } else {
            ++outCol;
        }
    }
}

void appendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void appendU64LE(std::vector<uint8_t>& buf, uint64_t v) {
    appendU32LE(buf, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    appendU32LE(buf, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

uint32_t readU32LE(std::span<const uint8_t> buf, size_t offset) noexcept {
    return static_cast<uint32_t>(buf[offset]) |
           (static_cast<uint32_t>(buf[offset + 1]) << 8) |
           (static_cast<uint32_t>(buf[offset + 2]) << 16) |
           (static_cast<uint32_t>(buf[offset + 3]) << 24);
}

uint64_t readU64LE(std::span<const uint8_t> buf, size_t offset) noexcept {
    uint64_t lo = readU32LE(buf, offset);
    uint64_t hi = readU32LE(buf, offset + 4);
    return lo | (hi << 32);
}

}  // namespace

uint64_t computeStateFnv1a64(std::span<const uint8_t> bytes) noexcept {
    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t hash = kFnvOffsetBasis;
    for (uint8_t b : bytes) {
        hash ^= static_cast<uint64_t>(b);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string formatChecksumHex(uint64_t checksum) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(checksum));
    return std::string(buf, 16);
}

DeterminismAuditResult verifyRollbackTickSource(std::string_view tickSource) {
    DeterminismAuditResult out;
    if (tickSource.empty()) {
        out.determinismLintPassed = false;
        out.errorSummary = "RollbackSession tick function source is empty";
        return out;
    }

    std::vector<bronze::check::CheckDiagnostic> diags;
    std::string parseErr;
    std::string effectiveSrc(tickSource);
    uint32_t prefixBytes = 0;

    bool parsed = bronze::check::checkDeterministicSource(effectiveSrc, diags, &parseErr);
    if (!parsed) {
        // Function.prototype.toString() on an anonymous `function (state, inputs) { ... }`
        // is a function expression rather than a top-level declaration; wrap it in a
        // variable binding so the module parser accepts it.
        diags.clear();
        parseErr.clear();
        const std::string prefix = "const __bro_rollback_tick = (";
        const std::string suffix = "\n);";
        effectiveSrc = prefix + std::string(tickSource) + suffix;
        prefixBytes = static_cast<uint32_t>(prefix.size());
        parsed = bronze::check::checkDeterministicSource(effectiveSrc, diags, &parseErr);
    }

    if (!parsed) {
        out.determinismLintPassed = false;
        out.errorSummary = "Failed to parse rollback tick function for determinism verification: " + parseErr;
        DeterminismHazard h;
        h.kind = "parse-error";
        h.message = out.errorSummary;
        h.line = 1;
        h.column = 1;
        out.hazards.push_back(std::move(h));
        return out;
    }

    for (const auto& d : diags) {
        DeterminismHazard h;
        h.kind = bronze::check::deterministicHazardTag(d.kind);
        h.message = d.message;
        h.remediation = d.remediation;
        h.functionName = d.functionName;
        uint32_t rawOffset = (d.span.begin >= prefixBytes) ? (d.span.begin - prefixBytes) : 0u;
        offsetToLineCol(tickSource, rawOffset, h.line, h.column);
        out.hazards.push_back(std::move(h));
    }

    if (out.hazards.empty()) {
        out.determinismLintPassed = true;
        out.certificateHash = computeSha256Hex("bronze-lockstep-cert-v1:" + std::string(tickSource));
    } else {
        out.determinismLintPassed = false;
        std::ostringstream oss;
        oss << "RollbackSession rejected non-deterministic tick function ("
            << out.hazards.size() << " lockstep hazard"
            << (out.hazards.size() == 1 ? "" : "s") << " detected): ";
        for (size_t i = 0; i < out.hazards.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << "[" << out.hazards[i].kind << "] " << out.hazards[i].message
                << " (line " << out.hazards[i].line << ")";
        }
        out.errorSummary = oss.str();
    }

    return out;
}

namespace {

void scanCapsuleStaticSafetyHazards(std::string_view src,
                                    std::vector<DeterminismHazard>& hazards) {
    auto isIdentChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '$';
    };

    struct ForbiddenToken {
        std::string_view token;
        const char* message;
        const char* remediation;
    };
    static constexpr ForbiddenToken kForbidden[] = {
        {"eval(", "dynamic code evaluation via eval() is forbidden in P2P capsules",
         "use static pure functions inside verified capsules"},
        {"Function(", "dynamic code construction via Function() is forbidden in P2P capsules",
         "use static pure functions inside verified capsules"},
        {"fetch(", "network I/O via fetch() is forbidden in deterministic P2P capsules",
         "pass external data explicitly via capsule state/inputs"},
        {"WebSocket", "WebSocket I/O is forbidden in deterministic P2P capsules",
         "use bro.net host transport outside the capsule tick"},
        {"XMLHttpRequest", "network I/O is forbidden in deterministic P2P capsules",
         "use pure deterministic computation inside capsules"},
    };

    for (const auto& f : kForbidden) {
        size_t pos = 0;
        while ((pos = src.find(f.token, pos)) != std::string_view::npos) {
            bool leftOk = (pos == 0) || !isIdentChar(src[pos - 1]);
            if (leftOk) {
                DeterminismHazard h;
                h.kind = "sandbox-escape";
                h.message = f.message;
                h.remediation = f.remediation;
                h.functionName = "<capsule>";
                offsetToLineCol(src, static_cast<uint32_t>(pos), h.line, h.column);
                hazards.push_back(std::move(h));
            }
            pos += f.token.size();
        }
    }

    // Check literal division or remainder by zero (`/ 0` or `% 0`)
    for (size_t i = 0; i + 1 < src.size(); ++i) {
        if (src[i] == '/' && (src[i + 1] == '/' || src[i + 1] == '*')) {
            if (src[i + 1] == '/') {
                while (i < src.size() && src[i] != '\n') ++i;
            } else {
                i += 2;
                while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            }
            continue;
        }
        if (src[i] == '/' || src[i] == '%') {
            size_t j = i + 1;
            while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
            if (j < src.size() && src[j] == '0') {
                char next = (j + 1 < src.size()) ? src[j + 1] : '\0';
                if (next != '.' && next != 'x' && next != 'X' && next != 'b' && next != 'B' &&
                    !(next >= '0' && next <= '9')) {
                    DeterminismHazard h;
                    h.kind = "division-by-zero";
                    h.message = "potential division or modulo by literal zero in capsule";
                    h.remediation = "guard divisor with `divisor !== 0` before dividing";
                    h.functionName = "<capsule>";
                    offsetToLineCol(src, static_cast<uint32_t>(i), h.line, h.column);
                    hazards.push_back(std::move(h));
                }
            }
        }
    }
}

std::string buildCapsuleCertHash(std::string_view entryName,
                                 std::string_view authorId,
                                 bool determinismLintPassed,
                                 bool hazardFree,
                                 std::string_view sha256) {
    std::string payload = "bro-capsule-cert-v1|";
    payload.append(entryName);
    payload.push_back('|');
    payload.append(authorId);
    payload.push_back('|');
    payload.push_back(determinismLintPassed ? '1' : '0');
    payload.push_back('|');
    payload.push_back(hazardFree ? '1' : '0');
    payload.push_back('|');
    payload.append(sha256);
    return computeSha256Hex(payload);
}

}  // namespace

VerifiedCapsule createVerifiedCapsule(std::string_view source,
                                      std::string_view entryName,
                                      std::string_view authorId) {
    VerifiedCapsule cap;
    cap.version = 1;
    cap.source = std::string(source);
    cap.entryName = entryName.empty() ? "capsule" : std::string(entryName);
    cap.authorId = authorId.empty() ? "peer-0" : std::string(authorId);
    cap.sha256 = computeSha256Hex(source);

    DeterminismAuditResult audit = verifyRollbackTickSource(source);
    cap.hazards = std::move(audit.hazards);
    scanCapsuleStaticSafetyHazards(source, cap.hazards);

    cap.hazardCount = static_cast<uint32_t>(cap.hazards.size());
    cap.hazardFree = cap.hazards.empty();
    cap.determinismLintPassed = audit.determinismLintPassed && cap.hazardFree;

    if (cap.determinismLintPassed) {
        cap.certificateHash = buildCapsuleCertHash(
            cap.entryName, cap.authorId, cap.determinismLintPassed, cap.hazardFree, cap.sha256);
    } else {
        cap.certificateHash = buildCapsuleCertHash(
            cap.entryName, cap.authorId, false, false, cap.sha256);
        if (!audit.errorSummary.empty()) {
            cap.errorSummary = audit.errorSummary;
        } else if (!cap.hazards.empty()) {
            std::ostringstream oss;
            oss << "Capsule rejected (" << cap.hazards.size() << " safety/determinism hazard"
                << (cap.hazards.size() == 1 ? "" : "s") << "): ["
                << cap.hazards[0].kind << "] " << cap.hazards[0].message;
            cap.errorSummary = oss.str();
        } else {
            cap.errorSummary = "Capsule verification failed";
        }
    }
    return cap;
}

CapsuleVerifyResult verifyCapsule(const VerifiedCapsule& capsule,
                                  const CapsulePolicy& policy) {
    CapsuleVerifyResult res;
    if (capsule.source.empty()) {
        res.valid = false;
        res.reason = "Capsule source is empty";
        return res;
    }

    VerifiedCapsule recomputed = createVerifiedCapsule(
        capsule.source, capsule.entryName, capsule.authorId);

    res.determinismLintPassed = recomputed.determinismLintPassed;
    res.hazardFree = recomputed.hazardFree;
    res.hazardCount = recomputed.hazardCount;
    res.hazards = recomputed.hazards;
    res.sha256 = recomputed.sha256;
    res.certificateHash = recomputed.certificateHash;

    if (!capsule.sha256.empty() && capsule.sha256 != recomputed.sha256) {
        res.valid = false;
        res.tampered = true;
        res.reason = "Capsule SHA-256 digest mismatch: source code was tampered with in transit";
        return res;
    }

    if (!capsule.certificateHash.empty() && capsule.certificateHash != recomputed.certificateHash) {
        res.valid = false;
        res.tampered = true;
        res.reason = "Capsule certificateHash mismatch: proof certificate forged or source modified";
        return res;
    }

    if ((capsule.determinismLintPassed && !recomputed.determinismLintPassed) ||
        (capsule.hazardFree && !recomputed.hazardFree)) {
        res.valid = false;
        res.tampered = true;
        res.reason = "Capsule proof metadata falsified: claimed verified on hazardous source";
        return res;
    }

    if (!policy.expectedSha256.empty() && policy.expectedSha256 != recomputed.sha256) {
        res.valid = false;
        res.tampered = true;
        res.reason = "Capsule SHA-256 does not match policy.expectedSha256";
        return res;
    }

    if (!policy.expectedCertificateHash.empty() &&
        policy.expectedCertificateHash != recomputed.certificateHash) {
        res.valid = false;
        res.tampered = true;
        res.reason = "Capsule certificateHash does not match policy.expectedCertificateHash";
        return res;
    }

    if (policy.requireDeterminismLint && !recomputed.determinismLintPassed) {
        res.valid = false;
        res.reason = recomputed.errorSummary;
        return res;
    }

    if (policy.requireHazardFree && !recomputed.hazardFree) {
        res.valid = false;
        res.reason = recomputed.errorSummary;
        return res;
    }

    res.valid = true;
    res.tampered = false;
    res.reason = "verified";
    return res;
}

RollbackSession::RollbackSession(const RollbackConfig& config, TickFn tickFn)
    : playerCount_(std::max<uint32_t>(1u, config.playerCount)),
      localPlayerId_(std::min<uint32_t>(config.localPlayerId, std::max<uint32_t>(1u, config.playerCount) - 1u)),
      maxRollbackFrames_(std::max<uint32_t>(4u, config.maxRollbackFrames)),
      audit_(verifyRollbackTickSource(config.tickSource)),
      tickFn_(std::move(tickFn)) {

    if (!config.initialStateBytes.empty()) {
        initialStateBytes_ = config.initialStateBytes;
    } else {
        initialStateBytes_.assign(std::max<uint32_t>(4u, config.stateSize), 0u);
    }
    currentStateBytes_ = initialStateBytes_;

    // Allocate ring buffer with extra headroom so snapshots [currentFrame - maxRollbackFrames .. currentFrame]
    // never alias in the circular table.
    ring_.resize(static_cast<size_t>(maxRollbackFrames_) + 4u);
    lastConfirmedInput_.assign(playerCount_, 0);
    hasConfirmedInput_.assign(playerCount_, false);

    saveStateSnapshot(0u, currentStateBytes_);
    auto& slot0 = slotFor(0u);
    slot0.inputs.assign(playerCount_, 0);
    slot0.inputConfirmed.assign(playerCount_, 1u);
}

void RollbackSession::setStateBytes(std::span<const uint8_t> bytes) {
    currentStateBytes_.assign(bytes.begin(), bytes.end());
    saveStateSnapshot(currentFrame_, currentStateBytes_);
}

void RollbackSession::saveStateSnapshot(uint32_t frame, const std::vector<uint8_t>& bytes) {
    uint64_t cs = computeStateFnv1a64(bytes);
    std::string hex = formatChecksumHex(cs);
    currentChecksum_ = cs;
    currentChecksumHex_ = hex;

    RollbackSnapshot& snap = slotFor(frame);
    snap.frame = frame;
    snap.stateBytes = bytes;
    snap.checksum = cs;
    snap.checksumHex = std::move(hex);
    if (snap.inputs.size() != playerCount_) {
        snap.inputs.assign(playerCount_, 0);
        snap.inputConfirmed.assign(playerCount_, 0u);
    }
    snap.valid = true;
}

int32_t RollbackSession::predictedInputFor(uint32_t frame, uint32_t playerId) const noexcept {
    if (playerId >= playerCount_) return 0;
    // Search backward from `frame` for the most recent confirmed input for `playerId`.
    const size_t maxIdx = std::min<size_t>(frame, inputHistory_.size());
    for (size_t k = maxIdx; k > 0; --k) {
        const auto& rec = inputHistory_[k - 1];
        if (playerId < rec.confirmed.size() && rec.confirmed[playerId]) {
            return rec.inputs[playerId];
        }
    }
    if (hasConfirmedInput_[playerId]) {
        return lastConfirmedInput_[playerId];
    }
    return 0;
}

RollbackAdvanceResult RollbackSession::advanceFrameSingle(int32_t localInput) {
    std::vector<int32_t> inputs(playerCount_, 0);
    inputs[localPlayerId_] = localInput;
    const uint32_t mask = (1u << localPlayerId_);
    return advanceFrame(inputs, mask);
}

RollbackAdvanceResult RollbackSession::advanceFrame(std::span<const int32_t> providedInputs,
                                                    uint32_t providedMask) {
    const uint32_t f = currentFrame_;
    if (inputHistory_.size() <= f) {
        inputHistory_.resize(static_cast<size_t>(f) + 1u,
                             FrameInputRecord{std::vector<int32_t>(playerCount_, 0),
                                              std::vector<uint8_t>(playerCount_, 0u)});
    }

    auto& rec = inputHistory_[f];
    for (uint32_t p = 0; p < playerCount_; ++p) {
        if (rec.confirmed[p]) {
            // Already confirmed via an earlier receiveRemoteInput(f, p, ...)
            continue;
        }
        const bool wasProvided = (p < providedInputs.size()) && (((providedMask >> p) & 1u) != 0u);
        if (wasProvided) {
            rec.inputs[p] = providedInputs[p];
            if (p == localPlayerId_) {
                rec.confirmed[p] = 1u;
                lastConfirmedInput_[p] = providedInputs[p];
                hasConfirmedInput_[p] = true;
            }
        } else {
            rec.inputs[p] = predictedInputFor(f, p);
        }
    }

    // Store the inputs used to simulate frame `f` on snapshot `f`
    {
        auto& preSnap = slotFor(f);
        preSnap.inputs = rec.inputs;
        preSnap.inputConfirmed = rec.confirmed;
    }

    if (tickFn_) {
        tickFn_(currentStateBytes_, rec.inputs, f);
    }

    currentFrame_ = f + 1u;
    saveStateSnapshot(currentFrame_, currentStateBytes_);
    {
        auto& postSnap = slotFor(currentFrame_);
        postSnap.inputs = rec.inputs;
        postSnap.inputConfirmed = rec.confirmed;
    }

    RollbackAdvanceResult res;
    res.frame = currentFrame_;
    res.checksum = currentChecksum_;
    res.checksumHex = currentChecksumHex_;
    res.checksumLow32 = checksumLow32(currentChecksum_);
    return res;
}

RollbackRemoteInputResult RollbackSession::receiveRemoteInput(uint32_t frame,
                                                              uint32_t playerId,
                                                              int32_t actualInput,
                                                              std::string_view remoteChecksumHex) {
    RollbackRemoteInputResult out;
    out.currentFrame = currentFrame_;
    out.currentChecksum = currentChecksumHex_;
    out.desyncDetected = desyncDetected_;

    if (playerId >= playerCount_) {
        return out;
    }

    if (inputHistory_.size() <= frame) {
        inputHistory_.resize(static_cast<size_t>(frame) + 1u,
                             FrameInputRecord{std::vector<int32_t>(playerCount_, 0),
                                              std::vector<uint8_t>(playerCount_, 0u)});
    }

    const bool wasSimulated = (frame < currentFrame_);
    const int32_t prevSimulatedInput = inputHistory_[frame].inputs[playerId];

    inputHistory_[frame].inputs[playerId] = actualInput;
    inputHistory_[frame].confirmed[playerId] = 1u;
    lastConfirmedInput_[playerId] = actualInput;
    hasConfirmedInput_[playerId] = true;

    // Propagate updated prediction to subsequent unconfirmed frames for `playerId`.
    bool anySubsequentChanged = false;
    if (wasSimulated) {
        for (uint32_t f = frame + 1u; f < currentFrame_; ++f) {
            if (inputHistory_[f].confirmed[playerId]) {
                break;
            }
            if (inputHistory_[f].inputs[playerId] != actualInput) {
                inputHistory_[f].inputs[playerId] = actualInput;
                anySubsequentChanged = true;
            }
        }
    }

    if (wasSimulated && (actualInput != prevSimulatedInput || anySubsequentChanged)) {
        const RollbackSnapshot& restoreSnap = slotFor(frame);
        if (restoreSnap.valid && restoreSnap.frame == frame) {
            currentStateBytes_ = restoreSnap.stateBytes;
            for (uint32_t f = frame; f < currentFrame_; ++f) {
                auto& fSnap = slotFor(f);
                fSnap.inputs = inputHistory_[f].inputs;
                fSnap.inputConfirmed = inputHistory_[f].confirmed;

                if (tickFn_) {
                    tickFn_(currentStateBytes_, inputHistory_[f].inputs, f);
                }
                saveStateSnapshot(f + 1u, currentStateBytes_);
                auto& nextSnap = slotFor(f + 1u);
                nextSnap.inputs = inputHistory_[f].inputs;
                nextSnap.inputConfirmed = inputHistory_[f].confirmed;
            }
            out.rolledBack = true;
            out.framesResimulated = currentFrame_ - frame;
            rollbackCount_ += 1u;
            totalFramesResimulated_ += out.framesResimulated;
        }
    } else if (wasSimulated) {
        auto& fSnap = slotFor(frame);
        if (fSnap.valid && fSnap.frame == frame && playerId < fSnap.inputConfirmed.size()) {
            fSnap.inputConfirmed[playerId] = 1u;
        }
        if (frame + 1u <= currentFrame_) {
            auto& nextSnap = slotFor(frame + 1u);
            if (nextSnap.valid && nextSnap.frame == frame + 1u && playerId < nextSnap.inputConfirmed.size()) {
                nextSnap.inputConfirmed[playerId] = 1u;
            }
        }
    }

    if (!remoteChecksumHex.empty() && frame <= currentFrame_) {
        bool matchesFrame = false;
        const auto& snapAtFrame = slotFor(frame);
        if (snapAtFrame.valid && snapAtFrame.frame == frame && snapAtFrame.checksumHex == remoteChecksumHex) {
            matchesFrame = true;
        }
        if (!matchesFrame && frame + 1u <= currentFrame_) {
            const auto& snapAfterFrame = slotFor(frame + 1u);
            if (snapAfterFrame.valid && snapAfterFrame.frame == frame + 1u &&
                snapAfterFrame.checksumHex == remoteChecksumHex) {
                matchesFrame = true;
            }
        }
        if (!matchesFrame) {
            desyncDetected_ = true;
        }
    }

    out.currentFrame = currentFrame_;
    out.currentChecksum = currentChecksumHex_;
    out.desyncDetected = desyncDetected_;
    return out;
}

bool RollbackSession::verifyPeerChecksum(uint32_t frame, std::string_view remoteChecksumHex) {
    if (remoteChecksumHex.empty()) return false;
    const RollbackSnapshot* snap = getSnapshot(frame);
    if (!snap) {
        desyncDetected_ = true;
        return false;
    }
    if (snap->checksumHex == remoteChecksumHex) {
        return true;
    }
    // Also check frame + 1 if caller passed 0-based tick frame index while comparing post-tick checksum
    if (frame + 1u <= currentFrame_) {
        const RollbackSnapshot* postSnap = getSnapshot(frame + 1u);
        if (postSnap && postSnap->checksumHex == remoteChecksumHex) {
            return true;
        }
    }
    desyncDetected_ = true;
    return false;
}

const RollbackSnapshot* RollbackSession::getSnapshot(uint32_t frame) const noexcept {
    if (frame > currentFrame_) return nullptr;
    const RollbackSnapshot& snap = slotFor(frame);
    if (!snap.valid || snap.frame != frame) return nullptr;
    return &snap;
}

RollbackStats RollbackSession::getStats() const {
    RollbackStats s;
    s.currentFrame = currentFrame_;
    s.rollbackCount = rollbackCount_;
    s.totalFramesResimulated = totalFramesResimulated_;
    s.determinismLintPassed = audit_.determinismLintPassed;
    s.desyncDetectionEnabled = true;
    s.certificateHash = audit_.certificateHash;
    s.desyncDetected = desyncDetected_;
    return s;
}

std::vector<uint8_t> RollbackSession::exportReplay() const {
    std::vector<uint8_t> out;
    // Header: "BRRP" (4 bytes), version=1 (u32), playerCount (u32), currentFrame (u32),
    // initialStateSize (u32), expectedFinalChecksum (u64)
    out.push_back('B');
    out.push_back('R');
    out.push_back('R');
    out.push_back('P');
    appendU32LE(out, 1u);
    appendU32LE(out, playerCount_);
    appendU32LE(out, currentFrame_);
    appendU32LE(out, static_cast<uint32_t>(initialStateBytes_.size()));
    appendU64LE(out, currentChecksum_);

    out.insert(out.end(), initialStateBytes_.begin(), initialStateBytes_.end());
    for (uint32_t f = 0; f < currentFrame_; ++f) {
        for (uint32_t p = 0; p < playerCount_; ++p) {
            int32_t val = (f < inputHistory_.size() && p < inputHistory_[f].inputs.size())
                              ? inputHistory_[f].inputs[p]
                              : 0;
            appendU32LE(out, static_cast<uint32_t>(val));
        }
    }
    return out;
}

ReplayVerifyResult RollbackSession::verifyReplay(std::span<const uint8_t> replayBytes) const {
    ReplayVerifyResult res;
    res.determinismLintPassed = audit_.determinismLintPassed;
    if (replayBytes.size() < 28u ||
        replayBytes[0] != 'B' || replayBytes[1] != 'R' ||
        replayBytes[2] != 'R' || replayBytes[3] != 'P') {
        return res;
    }

    uint32_t version = readU32LE(replayBytes, 4);
    uint32_t players = readU32LE(replayBytes, 8);
    uint32_t frames = readU32LE(replayBytes, 12);
    uint32_t initSize = readU32LE(replayBytes, 16);
    uint64_t expectedCs = readU64LE(replayBytes, 20);
    if (version != 1u || players == 0u) return res;

    size_t needed = 28u + static_cast<size_t>(initSize) +
                    static_cast<size_t>(frames) * static_cast<size_t>(players) * sizeof(uint32_t);
    if (replayBytes.size() < needed) return res;

    std::vector<uint8_t> state(replayBytes.begin() + 28, replayBytes.begin() + 28 + initSize);
    size_t offset = 28u + static_cast<size_t>(initSize);
    std::vector<int32_t> frameInputs(players, 0);

    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t p = 0; p < players; ++p) {
            frameInputs[p] = static_cast<int32_t>(readU32LE(replayBytes, offset));
            offset += 4u;
        }
        if (tickFn_) {
            tickFn_(state, frameInputs, f);
        }
    }

    uint64_t actualCs = computeStateFnv1a64(state);
    res.framesSimulated = frames;
    res.expectedChecksum = formatChecksumHex(expectedCs);
    res.actualChecksum = formatChecksumHex(actualCs);
    res.ok = (actualCs == expectedCs);
    return res;
}

}  // namespace bro::net

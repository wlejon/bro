#pragma once
#include <atomic>
#include <memory>

namespace bro::js {

/// Single-owner in-flight gate for async model operations. Every ML binding
/// (lm, stt, tts, diar) serializes ops per model: the decoder + KV cache are
/// single-owner, so a second concurrent generate/transcribe/synthesize must
/// be rejected at launch, and the gate must release on the JS thread BEFORE
/// onDone fires so a callback can synchronously start the next op.
///
/// Copyable: a session wrapper copies its model's gate so both share one
/// atomic — sessions over shared weights serialize with the model and with
/// sibling sessions. This replaces the per-binding
/// `std::shared_ptr<std::atomic<bool>> busy` copies that each hand-rolled
/// the same CAS/store/load dance.
class ModelGate {
public:
    /// Claim the gate. False if an op is already in flight.
    bool tryClaim() {
        bool expected = false;
        return busy_->compare_exchange_strong(expected, true);
    }

    /// Release on the JS thread once the work thread has finished and joined.
    void release() { busy_->store(false, std::memory_order_release); }

    bool isBusy() const { return busy_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic<bool>> busy_ =
        std::make_shared<std::atomic<bool>>(false);
};

} // namespace bro::js

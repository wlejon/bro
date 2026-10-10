#pragma once

// An animated image (GIF, WebP) as the page shows it: its frames, its
// timeline, and the repaints it asks for.
//
// WHERE IT LIVES. The image store decodes an <img> / CSS url() source once
// (render/image_store.h); when the source is an animation, the decoded
// picture is its first frame (natural size, img.decode(), createImageBitmap
// and canvas consumers that want a still all see frame 0) and carries an
// AnimatedImage for the rest. Every element and background showing that
// source shares it, and so shares one timeline, as browsers do.
//
// TIMELINE. Time is the engine's animation clock (setImageAnimationClock:
// bro.time's scaled clock, virtual time under headless advanceTime), so a
// paused bro.time pauses animations and headless runs are deterministic. The
// timeline starts the first time a frame is painted; each frame shows for its
// stored delay, a delay of 10 ms or less showing for 100 ms as in browsers;
// the loop count is honoured (it then rests on the last frame).
//
// REPAINTS. Only a painted animation asks for a repaint: painting a frame
// records when the next one is due, the engine repaints the documents when
// that time comes (takeDueImageAnimations), and the repaint, if it paints the
// animation again, records the next. One that is not painted — off screen,
// clipped out, display:none, detached, a hidden window — records nothing, so
// an idle page with nothing animating on screen requests no frames and burns
// no CPU. Its timeline keeps the clock: when it is painted again it shows the
// frame that time has reached (a streamed animation more than a window behind
// resumes from where it stopped instead of decoding its way there).
//
// FRAMES AND MEMORY (the budget). An animation whose frames all fit in the
// animation budget — BRO_IMAGE_ANIM_MB, default 32 MB of RGBA per animation —
// decodes every frame once, on the decoder thread that decoded the first, and
// keeps them. A bigger one is streamed: a sequential decoder
// (broimage::FrameDecoder, which holds one composed canvas) runs on the image
// store's decoder threads and keeps a decode-ahead window of up to 8 frames
// within the same budget (at least 2), refilled as frames are shown. Frames
// count against the image store's cache budget like any picture.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace broimage { class FrameDecoder; }

namespace bro::render {

struct DecodedImage;

// The per-animation budget in bytes (BRO_IMAGE_ANIM_MB; set for tests).
size_t animationBudgetBytes();
void setAnimationBudgetBytes(size_t bytes);

// Browsers' clamp: a delay of 10 ms or less shows for 100 ms.
int effectiveFrameDelayMs(int storedMs);

class AnimatedImage : public std::enable_shared_from_this<AnimatedImage> {
public:
    // On a decoder thread: `dec` has produced frame 0 (the owning
    // DecodedImage's pixels) and has more. Caches the rest now when they fit
    // the budget, else starts streaming.
    static std::shared_ptr<AnimatedImage> make(std::unique_ptr<broimage::FrameDecoder> dec);
    ~AnimatedImage();

    int frameCount() const;
    int loopCount() const;  // plays; 0 = forever
    bool cachesAll() const { return cachesAll_; }
    // The most it holds decoded (what the store counts).
    size_t budgetBytes() const { return budgetBytes_; }

    // The frame showing at `nowMs`. `painted`: it is being painted — starts
    // the timeline and schedules the repaint for the next frame. `wait`: a
    // streamed frame not decoded yet is waited for (headless paints)
    // instead of the latest decoded one standing in. Null means frame 0
    // (the owning DecodedImage itself).
    std::shared_ptr<const DecodedImage> frameAt(double nowMs, bool painted, bool wait);
    // The index frameAt last returned.
    int currentIndex() const;

    AnimatedImage(const AnimatedImage&) = delete;
    AnimatedImage& operator=(const AnimatedImage&) = delete;

private:
    AnimatedImage() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool cachesAll_ = false;
    size_t budgetBytes_ = 0;
    void scheduleDecodeLocked();
    void decodeAhead();
};

// The animation clock (ms). The engine installs its clock as the source
// (read on the page thread: painting, the frame pump) and also sets it each
// frame; other threads see the last value read or set.
void setImageAnimationClockSource(std::function<double()> source);
void setImageAnimationClock(double nowMs);
double imageAnimationClock();

// When the earliest painted animation wants its next frame (on the clock),
// or +infinity when none does.
double nextImageAnimationDueMs();
// Drop the deadlines that `nowMs` reached; true when there were any — the
// documents repaint, and repainting an animation schedules its next frame.
bool takeDueImageAnimations(double nowMs);

struct ImageAnimationStats {
    size_t live = 0;            // animations alive
    size_t scheduled = 0;       // animations waiting for a repaint
    uint64_t paints = 0;        // frames painted (frameAt with painted)
    uint64_t framesDecoded = 0; // frames decoded after the first
    uint64_t repaints = 0;      // takeDueImageAnimations calls that fired
};
ImageAnimationStats imageAnimationStats();

}  // namespace bro::render

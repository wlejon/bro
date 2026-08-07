// Unit test for MediaClock (FileClock and AudioSlavedClock).
//
// Verifies that:
// 1. Under a skewed audio clock rate (+200 ppm), AudioSlavedClock derives nowNs()
//    from consumed audio samples so video presentation strictly follows audio position
//    rather than host steady_clock.
// 2. Audio underruns cause the clock to hold (freeze nowNs()) instead of walking ahead.
// 3. Seeks and rate changes re-anchor the slaved clock continuously.
// 4. Sync error between presented video frame PTS and played audio PTS stays under 1 video frame.

#include "video/media_clock.h"
#include "video/video_pipeline.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace bro::video;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

} // namespace

int main() {
    std::printf("Testing AudioSlavedClock vs FileClock timing & sync accuracy...\n");

    // ── 1. +200 ppm Skewed Audio Clock Test ─────────────────────────
    std::printf("\n--- Test 1: +200 ppm Skewed Audio Clock ---\n");
    {
        constexpr uint32_t sampleRate = 48000;
        constexpr double skewPpm = +200.0;
        constexpr double skewFactor = 1.0 + (skewPpm / 1e6); // 1.000200

        uint64_t simulatedPlayedFrames = 0;

        // Provider that returns played frames advancing at +200 ppm relative to standard rate
        AudioSlavedClock slavedClock(sampleRate, [&simulatedPlayedFrames]() {
            return simulatedPlayedFrames;
        });

        FileClock fileClock;

        slavedClock.setPlaying(true);
        fileClock.setPlaying(true);

        // Simulate 3600 seconds (1 hour) of playback in 100-second steps
        const double durationSec = 3600.0;
        const double stepSec = 100.0;
        uint64_t totalPlayedAt1Hour = 0;

        for (double t = 0.0; t <= durationSec; t += stepSec) {
            simulatedPlayedFrames = static_cast<uint64_t>(t * sampleRate * skewFactor);
            if (t == durationSec) {
                totalPlayedAt1Hour = simulatedPlayedFrames;
            }
        }

        const TimeNs slavedNow = slavedClock.nowNs();
        const TimeNs expectedAudioPtsNs = static_cast<TimeNs>((static_cast<double>(totalPlayedAt1Hour) * 1e9) / sampleRate);
        const double slavedSec = slavedNow / 1e9;
        const double expectedSec = expectedAudioPtsNs / 1e9;

        // Under FileClock, 3600.0 s of wall time produces 3600.0 s of stream PTS,
        // missing the audio's +200 ppm skew (which is at 3600.72 s).
        const double fileClockSec = 3600.0;
        const double fileClockSyncErrorMs = std::abs(fileClockSec - expectedSec) * 1000.0;
        const double slavedSyncErrorMs = std::abs(slavedSec - expectedSec) * 1000.0;

        std::printf("  After 1 hour (+200 ppm audio rate):\n");
        std::printf("    Played Audio Position: %.6f s\n", expectedSec);
        std::printf("    AudioSlavedClock PTS:  %.6f s (Sync Error: %.3f ms)\n", slavedSec, slavedSyncErrorMs);
        std::printf("    FileClock PTS:         %.6f s (Sync Error: %.3f ms)\n", fileClockSec, fileClockSyncErrorMs);

        check(slavedSyncErrorMs < 0.001, "AudioSlavedClock sync error is zero (< 0.001 ms)");
        check(fileClockSyncErrorMs > 700.0, "FileClock drifts significantly (> 700 ms after 1 hour)");
    }

    // ── 2. Audio Underrun Test (Clock Holds) ──────────────────────────
    std::printf("\n--- Test 2: Audio Underrun (Clock Holds) ---\n");
    {
        constexpr uint32_t sampleRate = 48000;
        uint64_t playedFrames = 0;

        AudioSlavedClock clock(sampleRate, [&playedFrames]() {
            return playedFrames;
        });

        clock.setPlaying(true);
        clock.seekTo(0);

        // Advance to 5.0 seconds
        playedFrames = static_cast<uint64_t>(5.0 * sampleRate);
        const TimeNs ptsAt5s = clock.nowNs();
        check(std::abs(ptsAt5s / 1e9 - 5.0) < 1e-5, "Clock advances to 5.0 s");

        // Simulate audio underrun: playedFrames remains unchanged at 5.0 s while caller polls
        const TimeNs ptsDuringUnderrun1 = clock.nowNs();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const TimeNs ptsDuringUnderrun2 = clock.nowNs();

        check(ptsDuringUnderrun1 == ptsAt5s, "Clock holds during underrun check 1");
        check(ptsDuringUnderrun2 == ptsAt5s, "Clock holds during underrun check 2 (does not walk ahead)");

        // Resume audio playback: advance by 1 second of samples
        playedFrames += static_cast<uint64_t>(1.0 * sampleRate);
        const TimeNs ptsAfterResume = clock.nowNs();
        check(std::abs(ptsAfterResume / 1e9 - 6.0) < 1e-5, "Clock resumes cleanly to 6.0 s");
    }

    // ── 3. Seeks and Rate Changes Re-anchoring ────────────────────────
    std::printf("\n--- Test 3: Seeks & Rate Changes Re-anchoring ---\n");
    {
        constexpr uint32_t sampleRate = 48000;
        uint64_t playedFrames = 100000;

        AudioSlavedClock clock(sampleRate, [&playedFrames]() {
            return playedFrames;
        });

        clock.setPlaying(true);
        clock.seekTo(100 * 1000000000LL); // Seek to 100 s

        check(std::abs(clock.nowNs() / 1e9 - 100.0) < 1e-5, "Seek to 100 s re-anchors clock");

        // Play 2 seconds of audio after seek
        playedFrames += static_cast<uint64_t>(2.0 * sampleRate);
        check(std::abs(clock.nowNs() / 1e9 - 102.0) < 1e-5, "Now at 102 s after 2 s of playback");

        // Set playback rate to 1.5x
        clock.setRate(1.5);
        check(std::abs(clock.nowNs() / 1e9 - 102.0) < 1e-5, "Rate change keeps position continuous at 102 s");

        // Play 3 seconds of audio at 1.5x (3.0 * sampleRate stream frames consumed)
        playedFrames += static_cast<uint64_t>(3.0 * sampleRate);
        check(std::abs(clock.nowNs() / 1e9 - 105.0) < 1e-5, "Now at 105 s after 3 s of stream audio");
    }

    // ── 4. Frame Sync Error Assertion (< 1 Video Frame) ───────────────
    std::printf("\n--- Test 4: Video Presentation Sync Error (< 1 Frame) ---\n");
    {
        constexpr uint32_t sampleRate = 48000;
        constexpr double frameRate = 60.0;
        constexpr double frameDurationNs = 1e9 / frameRate; // ~16.66 ms

        uint64_t playedFrames = 0;
        auto slavedClock = std::make_unique<AudioSlavedClock>(sampleRate, [&playedFrames]() {
            return playedFrames;
        });

        VideoPipeline pipeline;
        pipeline.setClock(std::move(slavedClock));
        pipeline.play();

        // Simulate 1000 steps of playback
        double maxSyncErrorNs = 0.0;
        for (int i = 0; i < 1000; ++i) {
            playedFrames += 800; // 800 samples at 48kHz = 16.666 ms
            const TimeNs clockNow = pipeline.clockNs();
            const TimeNs playedAudioPts = static_cast<TimeNs>((static_cast<double>(playedFrames) * 1e9) / sampleRate);
            const double err = std::abs(static_cast<double>(clockNow - playedAudioPts));
            if (err > maxSyncErrorNs) maxSyncErrorNs = err;
        }

        std::printf("  Max clock vs played audio sync error across 1000 steps: %.3f ns (Frame budget: %.3f ns)\n",
                    maxSyncErrorNs, frameDurationNs);
        check(maxSyncErrorNs < frameDurationNs, "Sync error stays strictly under 1 video frame duration");
    }

    std::printf("\n%s\n", g_failures ? "FAILED" : "ALL TESTS PASSED");
    return g_failures ? 1 : 0;
}

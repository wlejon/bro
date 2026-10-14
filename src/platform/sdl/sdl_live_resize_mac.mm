// The modal-loop hook on macOS (EventLoop::setModalWindowEventHook). A window
// move is the window server's and blocks nothing; a live resize runs AppKit's
// tracking loop inside SDL_PumpEvents until the mouse comes up. A timer in the
// common run-loop modes fires inside that loop, as SDL's own live-resize timer
// does; this one calls the hook directly, where SDL's reports through its
// event watches with the watcher lock held (sdl_event_loop.cpp says why that
// will not do).
#include <SDL3/SDL.h>

#import <AppKit/AppKit.h>

#include <functional>

#if !__has_feature(objc_arc)
#error "sdl_live_resize_mac.mm is built with ARC (-fobjc-arc)"
#endif

namespace {
const std::function<void()>* g_hook = nullptr;
id g_willStart = nil;
id g_didEnd = nil;
NSTimer* g_timer = nil;

void stopTimer() {
    [g_timer invalidate];
    g_timer = nil;
}
}  // namespace

namespace bro::platform {

void sdlMacLiveResizeHook(const std::function<void()>* hook) {
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    g_hook = hook;
    if (hook && !g_willStart) {
        g_willStart = [center addObserverForName:NSWindowWillStartLiveResizeNotification
                                          object:nil
                                           queue:nil
                                      usingBlock:^(NSNotification*) {
            stopTimer();
            g_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                             repeats:YES
                                               block:^(NSTimer*) {
                if (g_hook && *g_hook) (*g_hook)();
            }];
            [[NSRunLoop currentRunLoop] addTimer:g_timer forMode:NSRunLoopCommonModes];
        }];
        g_didEnd = [center addObserverForName:NSWindowDidEndLiveResizeNotification
                                       object:nil
                                        queue:nil
                                   usingBlock:^(NSNotification*) { stopTimer(); }];
    } else if (!hook && g_willStart) {
        stopTimer();
        [center removeObserver:g_willStart];
        [center removeObserver:g_didEnd];
        g_willStart = nil;
        g_didEnd = nil;
    }
}

}  // namespace bro::platform

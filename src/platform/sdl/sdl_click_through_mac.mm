// The AppKit half of macOS click-through (sdl_click_through.cpp): a global
// monitor for pointer moves over other applications, and the window's
// ignoresMouseEvents. Monitor handlers run on the main thread, in the run
// loop SDL's event pump and wait drive.
#import <AppKit/AppKit.h>

#include <SDL3/SDL.h>

namespace bro::platform {

void* sdlMacWatchPointer(void (*onMove)(void*), void* ctx) {
    const NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                             NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
    id monitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                        handler:^(NSEvent*) { onMove(ctx); }];
    return (__bridge_retained void*)monitor;
}

void sdlMacUnwatchPointer(void* token) {
    id monitor = (__bridge_transfer id)token;
    if (monitor) [NSEvent removeMonitor:monitor];
}

void sdlMacSetIgnoresMouseEvents(SDL_Window* window, bool ignore) {
    NSWindow* nswindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (nswindow && nswindow.ignoresMouseEvents != ignore) nswindow.ignoresMouseEvents = ignore;
}

} // namespace bro::platform

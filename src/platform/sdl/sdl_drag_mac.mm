// Drags out of an SDL window on macOS: an NSDraggingSession on the window's
// content view, with NSPasteboardItems for what the page set. See sdl_drag.h.
//
// The session is not modal: it starts here and AppKit runs it from the event
// loop SDL pumps, reporting through the NSDraggingSource below. SDL's window
// is a drop target itself, so inside this application the source offers no
// operation at all (SDL's target refuses it) and a release over our own
// window is the page's own drop. AppKit swallows the release that ends the
// drag; a mouse-up is posted afterwards so SDL's button state catches up.
#include "platform/sdl/sdl_drag.h"

#include <SDL3/SDL.h>

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>

#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "sdl_drag_mac.mm is built with ARC (-fobjc-arc)"
#endif

using bro::platform::DragReport;

@interface BroDragSource : NSObject <NSDraggingSource>
@property(nonatomic, weak) NSWindow* window;
@property(nonatomic) uint32_t windowId;
@property(nonatomic) NSDragOperation outsideMask;
@property(nonatomic) BOOL inside;
@property(nonatomic) NSPoint last;
@end

namespace {
BroDragSource* g_source = nil;  // the session's source, alive until it ends
}

@implementation BroDragSource

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    return context == NSDraggingContextOutsideApplication ? self.outsideMask : NSDragOperationNone;
}

// Window coordinates (top-left origin, points) of a screen point over our
// window's content, or NO when the point is elsewhere or another window is
// on top there.
- (BOOL)windowPointFrom:(NSPoint)screen out:(NSPoint*)out {
    NSWindow* w = self.window;
    if (!w || !w.isVisible) return NO;
    if ([NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:0] != w.windowNumber) return NO;
    NSView* view = w.contentView;
    const NSPoint inWindow = [w convertRectFromScreen:NSMakeRect(screen.x, screen.y, 0, 0)].origin;
    const NSPoint inView = [view convertPoint:inWindow fromView:nil];
    if (!NSPointInRect(inView, view.bounds)) return NO;
    out->x = inView.x;
    out->y = view.isFlipped ? inView.y : view.bounds.size.height - inView.y;
    return YES;
}

- (void)draggingSession:(NSDraggingSession*)session movedToPoint:(NSPoint)screenPoint {
    (void)session;
    NSPoint p;
    if ([self windowPointFrom:screenPoint out:&p]) {
        if (!self.inside || p.x != self.last.x || p.y != self.last.y) {
            self.inside = YES;
            self.last = p;
            DragReport r;
            r.kind = DragReport::Kind::Motion;
            r.windowId = self.windowId;
            r.x = static_cast<float>(p.x);
            r.y = static_cast<float>(p.y);
            bro::platform::sdlDragReport(std::move(r));
        }
    } else if (self.inside) {
        self.inside = NO;
        DragReport r;
        r.kind = DragReport::Kind::Leave;
        r.windowId = self.windowId;
        bro::platform::sdlDragReport(std::move(r));
    }
}

- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    (void)session;
    // Escape ends a drag with no operation too; that is no drop.
    NSEvent* current = NSApp.currentEvent;
    const BOOL escaped = current && current.type == NSEventTypeKeyDown && current.keyCode == 53;
    NSPoint p;
    const BOOL own = operation == NSDragOperationNone && !escaped && [self windowPointFrom:screenPoint out:&p];

    const char* action = "none";
    if (operation & NSDragOperationMove) action = "move";
    else if (operation & (NSDragOperationCopy | NSDragOperationGeneric)) action = "copy";
    else if (operation & NSDragOperationLink) action = "link";

    bro::platform::sdlDragFinished(SDL_BUTTON_LEFT);
    if (own) {
        DragReport drop;
        drop.kind = DragReport::Kind::Drop;
        drop.windowId = self.windowId;
        drop.x = static_cast<float>(p.x);
        drop.y = static_cast<float>(p.y);
        bro::platform::sdlDragReport(std::move(drop));
    }
    DragReport end;
    end.action = action;
    bro::platform::sdlDragReport(std::move(end));

    // SDL never saw the release: post one, which it reports (and the event
    // loop drops, sdlTakeStaleButtonUp) as its button state catches up.
    if (NSWindow* w = self.window) {
        NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                         location:w.mouseLocationOutsideOfEventStream
                                    modifierFlags:0
                                        timestamp:NSProcessInfo.processInfo.systemUptime
                                     windowNumber:w.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:0];
        if (up) [NSApp postEvent:up atStart:NO];
    }
    g_source = nil;
}

@end

namespace bro::platform {

namespace {

NSString* nsString(const std::string& s) {
    NSString* str = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return str ? str : @"";
}

std::vector<std::string> uriListEntries(const std::string& list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find_first_of("\r\n", pos);
        std::string line = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? list.size() : end + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

// The pasteboard type (a UTI) for a MIME type: public.png for image/png, a
// dyn.* type that round-trips for one the system does not know.
NSString* utiForMime(const std::string& mime) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFStringRef uti = UTTypeCreatePreferredIdentifierForTag(kUTTagClassMIMEType,
                                                            (__bridge CFStringRef)nsString(mime), nullptr);
#pragma clang diagnostic pop
    return uti ? (__bridge_transfer NSString*)uti : nil;
}

NSImage* dragImage(const DragSource& drag) {
    if (drag.iconWidth <= 0 || drag.iconHeight <= 0 ||
        drag.iconBgra.size() < static_cast<size_t>(drag.iconWidth) * drag.iconHeight * 4)
        return nil;
    CFDataRef bytes = CFDataCreate(nullptr, drag.iconBgra.data(),
                                   static_cast<CFIndex>(drag.iconWidth) * drag.iconHeight * 4);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // Premultiplied BGRA in memory is 32-bit little-endian ARGB, alpha first.
    CGImageRef image = CGImageCreate(static_cast<size_t>(drag.iconWidth), static_cast<size_t>(drag.iconHeight), 8, 32,
                                     static_cast<size_t>(drag.iconWidth) * 4, space,
                                     kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst, provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(bytes);
    if (!image) return nil;
    NSImage* out = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(drag.iconWidth, drag.iconHeight)];
    CGImageRelease(image);
    return out;
}

}  // namespace

bool sdlNativeDragSupported() { return true; }

bool sdlNativeDragBegin(SDL_Window* window, uint32_t windowId, const DragSource& drag) {
    NSWindow* nswindow = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    NSView* view = nswindow.contentView;
    if (!nswindow || !view || g_source) return false;

    std::string text, uriList, html;
    bool haveText = false, haveUris = false, haveHtml = false;
    for (const auto& [mime, bytes] : drag.data) {
        if (mime == "text/plain" && !haveText) {
            text = bytes;
            haveText = true;
        } else if (mime == "text/uri-list" && !haveUris) {
            uriList = bytes;
            haveUris = true;
        } else if (mime == "text/html" && !haveHtml) {
            html = bytes;
            haveHtml = true;
        }
    }

    // One item per file (Finder takes a file per item); the first carries
    // everything else as well.
    NSMutableArray<NSPasteboardItem*>* items = [NSMutableArray array];
    NSPasteboardItem* first = [[NSPasteboardItem alloc] init];
    [items addObject:first];
    bool files = false;
    if (haveUris) {
        std::vector<std::string> urls;
        for (const auto& uri : uriListEntries(uriList)) {
            NSURL* url = [NSURL URLWithString:nsString(uri)];
            if (url && url.isFileURL) {
                NSPasteboardItem* item = files ? [[NSPasteboardItem alloc] init] : first;
                [item setString:url.absoluteString forType:NSPasteboardTypeFileURL];
                if (item != first) [items addObject:item];
                files = true;
            } else {
                urls.push_back(uri);
            }
        }
        if (!urls.empty()) {
            [first setString:nsString(urls.front()) forType:NSPasteboardTypeURL];
            if (!haveText) {
                text = urls.front();
                haveText = true;
            }
        }
    }
    if (haveText) [first setString:nsString(text) forType:NSPasteboardTypeString];
    if (haveHtml) [first setString:nsString(html) forType:NSPasteboardTypeHTML];
    for (const auto& [mime, bytes] : drag.data) {
        if (mime == "text/plain" || mime == "text/uri-list" || mime == "text/html") continue;
        // The X11 names the engine adds for text.
        if (mime == "UTF8_STRING" || mime == "TEXT" || mime == "STRING" || mime == "text/plain;charset=utf-8")
            continue;
        NSString* type = (mime == "text/rtf" || mime == "application/rtf") ? NSPasteboardTypeRTF : utiForMime(mime);
        if (!type || [first.types containsObject:type]) continue;
        [first setData:[NSData dataWithBytes:bytes.data() length:bytes.size()] forType:type];
    }

    // Where the picture goes: the icon's hot pixel under the pointer.
    const NSPoint inWindow = nswindow.mouseLocationOutsideOfEventStream;
    const NSPoint at = [view convertPoint:inWindow fromView:nil];
    NSImage* image = dragImage(drag);
    const CGFloat w = image ? drag.iconWidth : 1, h = image ? drag.iconHeight : 1;
    const CGFloat hx = image ? drag.hotX : 0, hy = image ? drag.hotY : 0;
    const NSRect frame = view.isFlipped ? NSMakeRect(at.x - hx, at.y - hy, w, h)
                                        : NSMakeRect(at.x - hx, at.y + hy - h, w, h);
    NSMutableArray<NSDraggingItem*>* dragging = [NSMutableArray array];
    for (NSPasteboardItem* item in items) {
        NSDraggingItem* d = [[NSDraggingItem alloc] initWithPasteboardWriter:item];
        [d setDraggingFrame:frame contents:image];
        [dragging addObject:d];
    }

    // The event the session starts from: a drag at the pointer, now.
    NSEvent* event = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                                        location:inWindow
                                   modifierFlags:NSEvent.modifierFlags
                                       timestamp:NSProcessInfo.processInfo.systemUptime
                                    windowNumber:nswindow.windowNumber
                                         context:nil
                                     eventNumber:0
                                      clickCount:1
                                        pressure:1.0];
    if (!event) return false;

    BroDragSource* source = [[BroDragSource alloc] init];
    source.window = nswindow;
    source.windowId = windowId;
    source.inside = YES;
    source.last = NSMakePoint(-1, -1);
    // A file goes to Finder as a copy unless only a move is allowed.
    NSDragOperation mask = NSDragOperationNone;
    if (drag.allowCopy) mask |= NSDragOperationCopy;
    if (drag.allowMove && !(files && drag.allowCopy)) mask |= NSDragOperationMove;
    if (mask == NSDragOperationNone) mask = NSDragOperationCopy;
    source.outsideMask = mask;

    NSDraggingSession* session = [view beginDraggingSessionWithItems:dragging event:event source:source];
    if (!session) return false;
    session.animatesToStartingPositionsOnCancelOrFail = YES;
    g_source = source;
    return true;
}

}  // namespace bro::platform

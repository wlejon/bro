// screen_grab.h on macOS: ScreenCaptureKit's SCScreenshotManager, one image
// per display, drawn into the requested rectangle of the desktop. It needs
// the Screen Recording permission (System Settings > Privacy & Security),
// which macOS grants to the executable: without it the grab is refused with
// that said, rather than handing back an image of only the wallpaper. The
// first refusal also asks macOS to show its prompt.
//
// Coordinates: CoreGraphics' global display space (origin at the main
// display's top-left, y down), times the largest display scale, so a Retina
// display is read at its full pixel resolution.
#include "platform/screen_grab.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>

namespace bro::platform::screen {

namespace {

constexpr int64_t kWaitNs = 5LL * 1000 * 1000 * 1000;

struct MacDisplay {
    CGDirectDisplayID id = 0;
    Display info;
};

double pixelScale(CGDirectDisplayID id, const CGRect& bounds) {
    double scale = 1.0;
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
        if (bounds.size.width > 0) scale = static_cast<double>(CGDisplayModeGetPixelWidth(mode)) / bounds.size.width;
        CGDisplayModeRelease(mode);
    }
    return scale > 0 ? scale : 1.0;
}

std::string displayName(CGDirectDisplayID id) {
    @autoreleasepool {
        for (NSScreen* s in [NSScreen screens]) {
            NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
            if (n && n.unsignedIntValue == id) return s.localizedName ? std::string(s.localizedName.UTF8String) : "";
        }
    }
    return {};
}

std::vector<MacDisplay> enumerate() {
    std::vector<MacDisplay> out;
    CGDirectDisplayID ids[32];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(32, ids, &n) != kCGErrorSuccess) return out;
    double maxScale = 1.0;
    for (uint32_t i = 0; i < n; ++i) maxScale = std::max(maxScale, pixelScale(ids[i], CGDisplayBounds(ids[i])));
    for (uint32_t i = 0; i < n; ++i) {
        const CGRect b = CGDisplayBounds(ids[i]);
        MacDisplay d;
        d.id = ids[i];
        d.info.name = displayName(ids[i]);
        d.info.x = static_cast<int>(std::lround(b.origin.x * maxScale));
        d.info.y = static_cast<int>(std::lround(b.origin.y * maxScale));
        d.info.width = static_cast<int>(std::lround(b.size.width * maxScale));
        d.info.height = static_cast<int>(std::lround(b.size.height * maxScale));
        d.info.scale = pixelScale(ids[i], b);
        d.info.primary = CGDisplayIsMain(ids[i]) != 0;
        out.push_back(d);
    }
    std::stable_sort(out.begin(), out.end(), [](const MacDisplay& a, const MacDisplay& b) {
        if (a.info.primary != b.info.primary) return a.info.primary;
        return a.info.x != b.info.x ? a.info.x < b.info.x : a.info.y < b.info.y;
    });
    for (size_t i = 0; i < out.size(); ++i) out[i].info.index = static_cast<int>(i);
    return out;
}

std::string permissionRefusal() {
    std::string exe;
    @autoreleasepool {
        NSString* p = [[NSProcessInfo processInfo] arguments].firstObject;
        if (p) exe = p.UTF8String;
    }
    return "macOS has not given this program the Screen Recording permission (System Settings > "
           "Privacy & Security > Screen Recording" +
           (exe.empty() ? std::string() : ", for " + exe) +
           "); a program started over ssh is judged by sshd's permission";
}

}  // namespace

std::vector<Display> displays(std::string* why) {
    std::vector<Display> list;
    for (const MacDisplay& d : enumerate()) list.push_back(d.info);
    if (list.empty() && why) *why = "no display is active";
    return list;
}

Status grab(int x, int y, int width, int height, Grab& out, std::string& why, bool cursor) {
    std::vector<MacDisplay> ds = enumerate();
    if (ds.empty()) {
        why = "no display is active";
        return Status::Unsupported;
    }
    if (@available(macOS 14.0, *)) {
    } else {
        why = "desktop capture needs macOS 14 or later (ScreenCaptureKit's SCScreenshotManager)";
        return Status::Unsupported;
    }
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();  // shows macOS's prompt, once
        why = permissionRefusal();
        return Status::Denied;
    }
    int vx0 = ds[0].info.x, vy0 = ds[0].info.y;
    int vx1 = vx0 + ds[0].info.width, vy1 = vy0 + ds[0].info.height;
    for (const MacDisplay& d : ds) {
        vx0 = std::min(vx0, d.info.x);
        vy0 = std::min(vy0, d.info.y);
        vx1 = std::max(vx1, d.info.x + d.info.width);
        vy1 = std::max(vy1, d.info.y + d.info.height);
    }
    if (width <= 0 || height <= 0) {
        x = vx0;
        y = vy0;
        width = vx1 - vx0;
        height = vy1 - vy0;
    }
    const int cx0 = std::max(x, vx0), cy0 = std::max(y, vy0);
    const int cx1 = std::min(x + width, vx1), cy1 = std::min(y + height, vy1);
    if (cx1 <= cx0 || cy1 <= cy0) {
        why = "the rectangle is outside the desktop";
        return Status::Failed;
    }
    out.x = cx0;
    out.y = cy0;
    out.width = cx1 - cx0;
    out.height = cy1 - cy0;
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;

    __block SCShareableContent* content = nil;
    __block NSError* contentErr = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent* c, NSError* e) {
                                                     content = c;
                                                     contentErr = e;
                                                     dispatch_semaphore_signal(sem);
                                                 }];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, kWaitNs)) != 0) {
        why = "ScreenCaptureKit did not answer";
        return Status::Failed;
    }
    if (!content) {
        why = contentErr ? std::string("ScreenCaptureKit: ") + contentErr.localizedDescription.UTF8String
                         : permissionRefusal();
        return contentErr && contentErr.code == SCStreamErrorUserDeclined ? Status::Denied : Status::Failed;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), out.width, out.height, 8,
                                             static_cast<size_t>(out.width) * 4, cs,
                                             kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        why = "could not create the capture bitmap";
        return Status::Failed;
    }
    bool any = false;
    std::string lastWhy;
    for (const MacDisplay& d : ds) {
        const int px0 = std::max(out.x, d.info.x), py0 = std::max(out.y, d.info.y);
        const int px1 = std::min(out.x + out.width, d.info.x + d.info.width);
        const int py1 = std::min(out.y + out.height, d.info.y + d.info.height);
        if (px1 <= px0 || py1 <= py0) continue;
        SCDisplay* scd = nil;
        for (SCDisplay* c in content.displays)
            if (c.displayID == d.id) scd = c;
        if (!scd) {
            lastWhy = "ScreenCaptureKit does not list display " + d.info.name;
            continue;
        }
        if (@available(macOS 14.0, *)) {
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:scd excludingWindows:@[]];
            SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
            cfg.width = static_cast<size_t>(d.info.width);
            cfg.height = static_cast<size_t>(d.info.height);
            cfg.showsCursor = cursor ? YES : NO;
            cfg.scalesToFit = YES;
            __block CGImageRef image = nullptr;
            __block NSError* shotErr = nil;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [SCScreenshotManager captureImageWithFilter:filter
                                          configuration:cfg
                                      completionHandler:^(CGImageRef img, NSError* e) {
                                          if (img) image = CGImageRetain(img);
                                          shotErr = e;
                                          dispatch_semaphore_signal(done);
                                      }];
            if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kWaitNs)) != 0) {
                lastWhy = "ScreenCaptureKit did not deliver the image";
                continue;
            }
            if (!image) {
                lastWhy = shotErr ? std::string("ScreenCaptureKit: ") + shotErr.localizedDescription.UTF8String
                                  : "ScreenCaptureKit delivered no image";
                continue;
            }
            // CoreGraphics draws y up; the bitmap's first row is the top.
            const CGRect dst = CGRectMake(d.info.x - out.x, out.height - (d.info.y - out.y) - d.info.height,
                                          d.info.width, d.info.height);
            CGContextDrawImage(ctx, dst, image);
            CGImageRelease(image);
            any = true;
        }
    }
    CGContextRelease(ctx);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    if (!any) {
        why = lastWhy.empty() ? "no display could be captured" : lastWhy;
        return Status::Failed;
    }
    out.backend = "screencapturekit";
    return Status::Ok;
}

}  // namespace bro::platform::screen

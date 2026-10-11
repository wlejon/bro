// macOS notifications through the UserNotifications framework
// (desktop_notifications.h). The framework serves only a process that is an
// application bundle with an identifier (a packaged bro.app); a bare `bro`
// binary has none, and showNotification falls back to AppleScript's
// `display notification` for it.

#include "platform/desktop_notifications.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <SDL3/SDL_events.h>

#include <functional>
#include <mutex>
#include <string>

// AppKit's NSApplicationLaunchIsDefaultLaunchKey for this run: NO when macOS
// started the app for something other than the user opening it, a click on
// a notification among them.
static bool g_defaultLaunch = true;

// The notification center's delegate: a click on a notification (or one of
// its actions) and its dismissal become activations for the page, from
// whichever run of the app is up when the user acts (macOS launches the app
// for a click when it is not running, and hands the response to the
// delegate set at launch).
API_AVAILABLE(macos(10.14))
@interface BroNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation BroNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler API_AVAILABLE(macos(10.14)) {
    namespace desktop = bro::platform::desktop;
    NSDictionary* info = response.notification.request.content.userInfo;
    NSString* args = info[@"bro"];
    NSString* appDir = info[@"broAppDir"];
    if (![args isKindOfClass:[NSString class]]) {
        completionHandler();
        return;
    }
    NSString* actionId = response.actionIdentifier;
    const bool close = [actionId isEqualToString:UNNotificationDismissActionIdentifier];
    std::string action;
    if (!close && ![actionId isEqualToString:UNNotificationDefaultActionIdentifier])
        action = std::string([actionId UTF8String]);
    std::string posted = [appDir isKindOfClass:[NSString class]] ? std::string([appDir UTF8String]) : std::string();
    std::string text([args UTF8String]);
    // On the main thread, where the activation queue and the launcher live
    // (SDL's Cocoa loop drains the main queue).
    dispatch_async(dispatch_get_main_queue(), ^{
        // Every bro app is this one bundle: the click may be another app's.
        const desktop::NotificationRoute route = desktop::routeNotificationResponse(posted, text, action, close);
        // A bare bro that macOS started only to deliver this click (no bro
        // was running) has handed it to its app and has nothing to show.
        if (route == desktop::NotificationRoute::Launched && desktop::notificationBareLaunch() && !g_defaultLaunch) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        completionHandler();
    });
}

// Shown while the app is in front too (the center hides them by default).
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
    API_AVAILABLE(macos(10.14)) {
    // Banner and List are macOS 11's (the bundle's LSMinimumSystemVersion);
    // the Alert they replaced is deprecated, and nothing older runs bro.
    UNNotificationPresentationOptions options = UNNotificationPresentationOptionSound;
    if (@available(macOS 11.0, *))
        options |= UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList;
    completionHandler(options);
}
@end

namespace bro::platform::desktop {

namespace {
std::mutex g_categoryMutex;
NSMutableSet* g_categories = nil;  // every action set this run posted with
}  // namespace

void initMacNotificationDelegate() {
    @autoreleasepool {
        if (![[NSBundle mainBundle] bundleIdentifier]) return;
        if (@available(macOS 10.14, *)) {
            static BroNotificationDelegate* delegate = nil;
            if (delegate) return;
            delegate = [[BroNotificationDelegate alloc] init];
            [UNUserNotificationCenter currentNotificationCenter].delegate = delegate;
            // Before SDL finishes launching the app, as the delegate is.
            [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification
                                                              object:nil
                                                               queue:nil
                                                          usingBlock:^(NSNotification* note) {
                NSNumber* isDefault = note.userInfo[NSApplicationLaunchIsDefaultLaunchKey];
                if ([isDefault isKindOfClass:[NSNumber class]]) g_defaultLaunch = [isDefault boolValue];
            }];
        }
    }
}

bool showMacUserNotification(const std::string& title, const std::string& body,
                             const NotificationOptions& options, uint32_t id) {
    @autoreleasepool {
        if (![[NSBundle mainBundle] bundleIdentifier]) return false;
        if (@available(macOS 10.14, *)) {
            initMacNotificationDelegate();
            UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
            if (!center) return false;
            UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
            content.title = [NSString stringWithUTF8String:title.c_str()] ?: @"";
            content.body = [NSString stringWithUTF8String:body.c_str()] ?: @"";
            if (!options.silent) content.sound = [UNNotificationSound defaultSound];
            // What the delegate hands back: this run, the id, the payload.
            NSString* args = [NSString stringWithUTF8String:encodeNotificationArgs(id, "", options.payload).c_str()];
            // And the app that posted it: every bro app is this one bundle,
            // so the bro that hears the click may be another app, or a bare
            // one macOS started for it (routeNotificationResponse).
            NSString* appDir = [NSString stringWithUTF8String:notificationLaunchAppDir().c_str()];
            content.userInfo = @{@"bro" : args ?: @"", @"broAppDir" : appDir ?: @""};
            // The buttons are a category of their own (one per distinct set),
            // with the dismissal reported (CustomDismissAction).
            std::string categoryKey = "bro";
            for (const auto& a : options.actions) categoryKey += "|" + a.id + "=" + a.title;
            NSString* categoryId = [NSString stringWithFormat:@"bro-%lu",
                                    static_cast<unsigned long>(std::hash<std::string>{}(categoryKey))];
            {
                std::lock_guard<std::mutex> lock(g_categoryMutex);
                if (!g_categories) g_categories = [[NSMutableSet alloc] init];
                BOOL known = NO;
                for (UNNotificationCategory* c in g_categories) {
                    if ([c.identifier isEqualToString:categoryId]) known = YES;
                }
                if (!known) {
                    NSMutableArray* actions = [NSMutableArray array];
                    for (const auto& a : options.actions) {
                        NSString* aid = [NSString stringWithUTF8String:a.id.c_str()] ?: @"";
                        NSString* atitle = [NSString stringWithUTF8String:a.title.c_str()] ?: @"";
                        [actions addObject:[UNNotificationAction actionWithIdentifier:aid
                                                                                title:atitle
                                                                              options:UNNotificationActionOptionForeground]];
                    }
                    [g_categories addObject:[UNNotificationCategory
                                                categoryWithIdentifier:categoryId
                                                               actions:actions
                                                     intentIdentifiers:@[]
                                                               options:UNNotificationCategoryOptionCustomDismissAction]];
                    [center setNotificationCategories:g_categories];
                }
            }
            content.categoryIdentifier = categoryId;
            // The id as the request's identifier: a later request with the
            // same one (replacesId) replaces it.
            NSString* identifier = [NSString stringWithFormat:@"bro-%u", id];
            UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                  content:content
                                                                                  trigger:nil];
            // Asking is a no-op once the user has answered; the first
            // notification asks, and shows if they allow it.
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                  completionHandler:^(BOOL granted, NSError* _Nullable) {
                                      if (granted) [center addNotificationRequest:request withCompletionHandler:nil];
                                  }];
            return true;
        }
        return false;
    }
}

}  // namespace bro::platform::desktop

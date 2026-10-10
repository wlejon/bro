// macOS notifications through the UserNotifications framework
// (desktop_notifications.h). The framework serves only a process that is an
// application bundle with an identifier (a packaged bro.app); a bare `bro`
// binary has none, and showNotification falls back to AppleScript's
// `display notification` for it.

#include "platform/desktop_notifications.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

namespace bro::platform::desktop {

bool showMacUserNotification(const std::string& title, const std::string& body,
                             const NotificationOptions& options, uint32_t id) {
    @autoreleasepool {
        if (![[NSBundle mainBundle] bundleIdentifier]) return false;
        if (@available(macOS 10.14, *)) {
            UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
            if (!center) return false;
            UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
            content.title = [NSString stringWithUTF8String:title.c_str()] ?: @"";
            content.body = [NSString stringWithUTF8String:body.c_str()] ?: @"";
            if (!options.silent) content.sound = [UNNotificationSound defaultSound];
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

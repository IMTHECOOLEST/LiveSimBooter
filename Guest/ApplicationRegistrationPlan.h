#pragma once

#import <Foundation/Foundation.h>

// These helpers only inspect files. They never contact LaunchServices, including when built for native boundary tests.
// The manifest has exactly {"version": 1, "applications": [absolute guest app paths]}; its own path is also guest-relative.
NSArray<NSString *> *LSBReadApplicationRegistrationManifest(NSString *root, NSString *manifestPath, NSError **error);
// A nil result with skipReason means an original resource-only app stub. Other failures populate error.
NSURL *LSBPrepareApplicationRegistration(NSString *root, NSString *guestPath, NSString **identifier, NSString **skipReason, NSError **error);
BOOL LSBVerifyApplicationRegistration(NSURL *expectedURL, NSString *expectedIdentifier, NSURL *registeredURL,
                                    NSString *registeredIdentifier, BOOL installed, NSError **error);

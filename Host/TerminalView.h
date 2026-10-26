#pragma once

#import <UIKit/UIKit.h>

@interface LSBTerminalView : UIView <UIKeyInput, UITextInputTraits>
@property (nonatomic, copy) void (^inputHandler)(NSData *data);
@property (nonatomic, copy) void (^resizeHandler)(unsigned short rows, unsigned short columns);
// Host lifecycle text stays outside the guest terminal stream, so a stale waiting message cannot survive attachment.
@property (nonatomic, copy) NSString *statusText;
- (void)appendOutput:(NSData *)data;
@end

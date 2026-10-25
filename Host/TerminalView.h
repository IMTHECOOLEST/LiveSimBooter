#pragma once

#import <UIKit/UIKit.h>

@interface LSBTerminalView : UIView <UIKeyInput, UITextInputTraits>
@property (nonatomic, copy) void (^inputHandler)(NSData *data);
@property (nonatomic, copy) void (^resizeHandler)(unsigned short rows, unsigned short columns);
- (void)appendOutput:(NSData *)data;
@end

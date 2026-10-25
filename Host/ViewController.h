#pragma once

#import <UIKit/UIKit.h>
@class LSBSimulatorDisplayView;
// Main-thread access for the native surface presenter.
LSBSimulatorDisplayView *LSBHostSimulatorDisplay(void);

@interface LSBHostViewController : UIViewController
@end

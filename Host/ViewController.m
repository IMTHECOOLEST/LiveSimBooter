#import "ViewController.h"
#include "HostApplication.h"
#include <string.h>

@implementation LSBHostViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    // The default simulator guest is spawn-probe, which performs posix_spawn through the full guest-to-host path.
    int startupError = LSBStartInitialGuest();
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *title = [UILabel new];
    title.text = @"LiveSimBooter";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleLargeTitle];
    title.adjustsFontForContentSizeCategory = YES;
    title.numberOfLines = 0;

    UILabel *status = [UILabel new];
    status.text = startupError ? [NSString stringWithFormat:@"Could not start runtime: %s", strerror(startupError)] : @"Runtime host ready";
    status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    status.adjustsFontForContentSizeCategory = YES;
    status.numberOfLines = 0;

    UILabel *instruction = [UILabel new];
    instruction.text = @"Keep this app open while your runtime programs run.";
    instruction.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    instruction.textColor = UIColor.secondaryLabelColor;
    instruction.adjustsFontForContentSizeCategory = YES;
    instruction.numberOfLines = 0;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, status, instruction]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:safe.topAnchor constant:24],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-24]
    ]];
}
@end

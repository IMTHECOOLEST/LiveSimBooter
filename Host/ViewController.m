#import "ViewController.h"
#include "HostApplication.h"
#include "Broker.h"
#import "TerminalSession.h"
#import "TerminalView.h"
#import "SimulatorDisplayView.h"
#import "FramebufferServer.h"
#include <errno.h>
#include <string.h>

static __weak LSBSimulatorDisplayView *hostDisplay;
LSBSimulatorDisplayView *LSBHostSimulatorDisplay(void) { return hostDisplay; }

@interface LSBHostViewController ()
@property (nonatomic, strong) id indigoObserver;
@property (nonatomic, strong) id framebufferObserver;
@property (nonatomic, weak) LSBSimulatorDisplayView *simulatorDisplay;
@property (nonatomic, strong) LSBFramebufferServer *framebuffer;
@property uint64_t displayGeneration;
@end

@implementation LSBHostViewController
- (void)dealloc {
    if (_indigoObserver) [NSNotificationCenter.defaultCenter removeObserver:_indigoObserver];
    if (_framebufferObserver) [NSNotificationCenter.defaultCenter removeObserver:_framebufferObserver];
    _framebuffer.frameHandler = nil;
}
- (void)bindFramebuffer {
    LSBFramebufferServer *server = LSBHostFramebufferServer();
    [self.simulatorDisplay resetPresentation];
    if (_framebuffer == server) return;
    _framebuffer.frameHandler = nil;
    _framebuffer = server;
    uint64_t generation = ++_displayGeneration;
    if (!server) return;
    __weak LSBHostViewController *weakSelf = self;
    __weak LSBFramebufferServer *weakServer = server;
    server.frameHandler = ^(IOSurfaceRef surface, CGRect dirtyRect, void (^completion)(int)) {
        LSBHostViewController *controller = weakSelf;
        if (!controller || controller.framebuffer != weakServer || controller.displayGeneration != generation) { completion(ECANCELED); return; }
        LSBSimulatorDisplayView *display = controller.simulatorDisplay;
        if (display) [display presentSurface:surface dirtyRect:dirtyRect completion:completion];
        else completion(ENODEV);
    };
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    for (UIView *view in self.view.subviews) if ([view isKindOfClass:LSBTerminalView.class]) [view becomeFirstResponder];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    int startupError = LSBStartInitialGuest();
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    if (!startupError && LSBHostUsesSimulatorDisplay()) {
        LSBSimulatorDisplayView *display = [[LSBSimulatorDisplayView alloc] initWithFrame:CGRectZero];
        display.translatesAutoresizingMaskIntoConstraints = NO;
        display.indigoServer = LSBHostIndigoHIDServer();
        [self.view addSubview:display];
        hostDisplay = display;
        self.simulatorDisplay = display;
        [self bindFramebuffer];
        __weak LSBSimulatorDisplayView *weakDisplay = display;
        self.indigoObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostIndigoDidChangeNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
                (void)notification;
                weakDisplay.indigoServer = LSBHostIndigoHIDServer();
            }];
        __weak LSBHostViewController *weakSelf = self;
        self.framebufferObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostFramebufferDidChangeNotification object:nil
            queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
                (void)notification;
                [weakSelf bindFramebuffer];
            }];
        UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
        // Keep the actual touch view at the guest screen's aspect, with a little space inside the native safe area.
        NSLayoutConstraint *preferredWidth = [display.widthAnchor constraintEqualToAnchor:safe.widthAnchor multiplier:0.94];
        preferredWidth.priority = UILayoutPriorityDefaultHigh;
        [NSLayoutConstraint activateConstraints:@[
            [display.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
            [display.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
            [display.widthAnchor constraintEqualToAnchor:display.heightAnchor multiplier:1290.0 / 2796.0],
            [display.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor multiplier:0.94],
            [display.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor multiplier:0.94],
            preferredWidth
        ]];
        return;
    }
    LSBTerminalSession *session = LSBHostTerminal();
    if (!startupError && session) {
        LSBTerminalView *terminal = [[LSBTerminalView alloc] initWithFrame:CGRectZero];
        terminal.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:terminal];
        UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [terminal.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
            [terminal.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
            [terminal.topAnchor constraintEqualToAnchor:safe.topAnchor],
            [terminal.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor]
        ]];
        __weak LSBTerminalView *weakTerminal = terminal;
        session.outputHandler = ^(NSData *data) { [weakTerminal appendOutput:data]; };
        terminal.inputHandler = ^(NSData *data) {
            int error = [session sendInput:data];
            if (error) fprintf(stderr, "[terminal] input: %s\n", strerror(error));
        };
        terminal.resizeHandler = ^(unsigned short rows, unsigned short columns) { [session resizeRows:rows columns:columns]; };
        [terminal becomeFirstResponder];
        return;
    }
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

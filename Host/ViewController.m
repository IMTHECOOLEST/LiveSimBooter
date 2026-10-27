#import "ViewController.h"
#include "HostApplication.h"
#include "Broker.h"
#import "TerminalSession.h"
#import "TerminalView.h"
#import "SimulatorDisplayView.h"
#import "FramebufferServer.h"
#import "SettingsViewController.h"
#import "BackgroundAudio.h"
#include <errno.h>
#include <string.h>

static __weak LSBSimulatorDisplayView *hostDisplay;
LSBSimulatorDisplayView *LSBHostSimulatorDisplay(void) { return hostDisplay; }

static void showStatus(UIViewController *controller, NSString *text) {
    UILabel *label = [UILabel new];
    label.text = text;
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [controller.view addSubview:label];
    UILayoutGuide *safe = controller.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [label.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [label.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor]
    ]];
}

@interface LSBSurfaceViewController : UIViewController
@property BOOL visible;
@property BOOL startupAttempted;
@property (nonatomic, copy) void (^startupResultHandler)(int);
@property (nonatomic, strong) UIStackView *startOverlay;
@property (nonatomic, strong) UILabel *startupStatus;
@property (nonatomic, strong) UIButton *startButton;
@property (nonatomic, strong) id indigoObserver;
@property (nonatomic, strong) id framebufferObserver;
@property (nonatomic, strong) LSBSimulatorDisplayView *simulatorDisplay;
@property (nonatomic, strong) NSLayoutConstraint *displayAspect;
@property (nonatomic, strong) LSBFramebufferServer *framebuffer;
@property uint64_t displayGeneration;
@end

@implementation LSBSurfaceViewController
- (void)dealloc {
    if (_indigoObserver) [NSNotificationCenter.defaultCenter removeObserver:_indigoObserver];
    if (_framebufferObserver) [NSNotificationCenter.defaultCenter removeObserver:_framebufferObserver];
    _framebuffer.frameHandler = nil;
}
- (void)bindFramebuffer {
    LSBFramebufferServer *server = LSBHostFramebufferServer();
    if (_framebuffer == server) return;
    [self.simulatorDisplay resetPresentation];
    _framebuffer.frameHandler = nil;
    _framebuffer = server;
    uint64_t generation = ++_displayGeneration;
    if (!server) return;
    LSBFramebufferGeometry geometry = server.geometry;
    self.displayAspect.active = NO;
    self.displayAspect = [self.simulatorDisplay.widthAnchor constraintEqualToAnchor:self.simulatorDisplay.heightAnchor
                                                                        multiplier:(CGFloat)geometry.width / geometry.height];
    self.displayAspect.active = YES;
    __weak LSBSurfaceViewController *weakSelf = self;
    __weak LSBFramebufferServer *weakServer = server;
    server.frameHandler = ^(IOSurfaceRef surface, CGRect dirtyRect, void (^completion)(int)) {
        LSBSurfaceViewController *controller = weakSelf;
        if (!controller || controller.framebuffer != weakServer || controller.displayGeneration != generation) { completion(ECANCELED); return; }
        LSBSimulatorDisplayView *display = controller.simulatorDisplay;
        // Hidden presentation still consumes dirty updates into its owned baseline without requesting a drawable.
        if (display) [display presentSurface:surface dirtyRect:dirtyRect completion:completion];
        else completion(ENODEV);
    };
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.visible = YES;
    self.simulatorDisplay.presentationEnabled = YES;
    self.simulatorDisplay.userInteractionEnabled = YES;
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self.simulatorDisplay becomeFirstResponder];
}
- (void)viewWillDisappear:(BOOL)animated {
    self.visible = NO;
    self.simulatorDisplay.userInteractionEnabled = NO; // Cancels active contacts before another tab can receive touches.
    self.simulatorDisplay.presentationEnabled = NO;
    [super viewWillDisappear:animated];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *status = [UILabel new];
    status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    status.adjustsFontForContentSizeCategory = YES;
    status.numberOfLines = 0;
    status.textAlignment = NSTextAlignmentCenter;
    status.hidden = YES;
    UIButtonConfiguration *configuration = UIButtonConfiguration.filledButtonConfiguration;
    configuration.title = @"Start";
    configuration.buttonSize = UIButtonConfigurationSizeLarge;
    __weak LSBSurfaceViewController *weakSelf = self;
    UIButton *button = [UIButton buttonWithConfiguration:configuration primaryAction:[UIAction actionWithHandler:^(UIAction *action) {
        (void)action;
        [weakSelf startInitialRuntime];
    }]];
    button.accessibilityIdentifier = @"runtime-start";
    UIStackView *overlay = [[UIStackView alloc] initWithArrangedSubviews:@[status, button]];
    overlay.axis = UILayoutConstraintAxisVertical;
    overlay.alignment = UIStackViewAlignmentCenter;
    overlay.spacing = 16;
    overlay.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:overlay];
    self.startOverlay = overlay;
    self.startupStatus = status;
    self.startButton = button;
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [overlay.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [overlay.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [overlay.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor]
    ]];
}
- (void)startInitialRuntime {
    if (self.startupAttempted) return;
    self.startupAttempted = YES;
    self.startButton.enabled = NO;
    int error = LSBStartInitialGuest();
    if (self.startupResultHandler) self.startupResultHandler(error);
    if (error) {
        self.startupStatus.text = [NSString stringWithFormat:@"Could not start runtime: %s. Close and reopen the app to try again.", strerror(error)];
        self.startupStatus.hidden = NO;
        self.startButton.hidden = YES;
        return;
    }
    [self.startOverlay removeFromSuperview];
    self.startOverlay = nil;
    self.startupStatus = nil;
    self.startButton = nil;
    if (!LSBHostUsesSimulatorDisplay()) { showStatus(self, @"This runtime has no simulator display."); return; }
    LSBSimulatorDisplayView *display = [[LSBSimulatorDisplayView alloc] initWithFrame:CGRectZero];
    display.translatesAutoresizingMaskIntoConstraints = NO;
    display.indigoServer = LSBHostIndigoHIDServer();
    display.presentationEnabled = self.visible;
    display.userInteractionEnabled = self.visible;
    [self.view addSubview:display];
    hostDisplay = display;
    self.simulatorDisplay = display;
    self.displayAspect = [display.widthAnchor constraintEqualToAnchor:display.heightAnchor multiplier:1290.0 / 2796.0];
    self.displayAspect.active = YES;
    [self bindFramebuffer];
    __weak LSBSimulatorDisplayView *weakDisplay = display;
    self.indigoObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostIndigoDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            weakDisplay.indigoServer = LSBHostIndigoHIDServer();
        }];
    __weak LSBSurfaceViewController *weakSelf = self;
    self.framebufferObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostFramebufferDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf bindFramebuffer]; }];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint *preferredWidth = [display.widthAnchor constraintEqualToAnchor:safe.widthAnchor multiplier:0.94];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [display.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [display.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [display.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor multiplier:0.94],
        [display.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor multiplier:0.94],
        preferredWidth
    ]];
    if (self.visible) [display becomeFirstResponder];
}
@end

@interface LSBFishViewController : UIViewController
@property int startupError;
@property BOOL visible;
@property BOOL receivedTerminalOutput;
@property (nonatomic, strong) id terminalObserver;
@property (nonatomic, strong) id closeObserver;
@property (nonatomic, strong) LSBTerminalView *terminal;
@property (nonatomic, strong) LSBTerminalSession *session;
@end

@implementation LSBFishViewController
- (void)dealloc {
    if (_terminalObserver) [NSNotificationCenter.defaultCenter removeObserver:_terminalObserver];
    if (_closeObserver) [NSNotificationCenter.defaultCenter removeObserver:_closeObserver];
    _session.outputHandler = nil;
}
- (NSString *)waitingStatus {
    if (self.startupError) return [NSString stringWithFormat:@"Runtime startup failed: %s. Close and reopen the app to try again.", strerror(self.startupError)];
    return LSBInitialGuestStartupWasAttempted() ? @"Waiting for launchd to start Fish…" : @"Start the simulator from the IOSurface tab.";
}
- (void)runtimeStartupFinished:(int)error {
    self.startupError = error;
    if (!self.session) self.terminal.statusText = [self waitingStatus];
}
- (void)bindTerminal {
    LSBTerminalSession *session = LSBHostTerminal();
    if (_session == session) {
        if (session.closed) [self terminalClosed];
        return;
    }
    _session.outputHandler = nil;
    _session = session;
    self.receivedTerminalOutput = NO;
    self.terminal.inputHandler = nil;
    self.terminal.resizeHandler = nil;
    self.terminal.statusText = session ? @"Terminal attached." : [self waitingStatus];
    if (!session) return;
    __weak LSBFishViewController *weakSelf = self;
    self.terminal.inputHandler = ^(NSData *data) {
        LSBFishViewController *controller = weakSelf;
        // Hidden tabs cannot receive UIKit input, but the terminal parser must still answer device/cursor queries from Fish.
        if (!controller || controller.session != session || session.closed) return;
        int error = [session sendInput:data];
        if (error) fprintf(stderr, "[terminal] input: %s\n", strerror(error));
    };
    self.terminal.resizeHandler = ^(unsigned short rows, unsigned short columns) { [session resizeRows:rows columns:columns]; };
    __weak LSBTerminalSession *weakSession = session;
    session.outputHandler = ^(NSData *data) {
        LSBFishViewController *controller = weakSelf;
        if (!controller || controller.session != weakSession) return;
        if (data.length) controller.receivedTerminalOutput = YES;
        [controller.terminal appendOutput:data];
    };
    [self.terminal setNeedsLayout];
    if (session.closed) [self terminalClosed];
    else if (self.visible) [self.terminal becomeFirstResponder];
}
- (void)terminalClosed {
    self.terminal.inputHandler = nil;
    self.terminal.resizeHandler = nil;
    [self.terminal resignFirstResponder];
    self.terminal.statusText = self.receivedTerminalOutput ? @"Terminal closed." : @"Terminal closed without output.";
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    LSBTerminalView *terminal = [[LSBTerminalView alloc] initWithFrame:CGRectZero];
    terminal.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:terminal];
    self.terminal = terminal;
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint *preferredBottom = [terminal.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor];
    preferredBottom.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [terminal.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [terminal.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [terminal.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [terminal.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor],
        [terminal.bottomAnchor constraintLessThanOrEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor],
        preferredBottom
    ]];
    terminal.statusText = [self waitingStatus];
    [self bindTerminal];
    __weak LSBFishViewController *weakSelf = self;
    self.terminalObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBHostTerminalDidChangeNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { (void)notification; [weakSelf bindTerminal]; }];
    self.closeObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBTerminalSessionDidCloseNotification object:nil
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            LSBFishViewController *controller = weakSelf;
            if (controller && notification.object == controller.session) [controller terminalClosed];
        }];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    self.visible = YES;
    // A tab/scene can become visible after attachment and EOF; recover the current session even if a notification was missed.
    [self bindTerminal];
    if (self.session && !self.session.closed) [self.terminal becomeFirstResponder];
}
- (void)viewWillDisappear:(BOOL)animated {
    self.visible = NO;
    [self.view endEditing:YES];
    [super viewWillDisappear:animated];
}
@end

@implementation LSBHostViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    // The scene keeps the display and terminal connections. Tab selection never starts or stops a process.
    LSBSurfaceViewController *surface = [LSBSurfaceViewController new];
    surface.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"IOSurface" image:[UIImage systemImageNamed:@"iphone"] tag:0];
    LSBFishViewController *fish = [LSBFishViewController new];
    __weak LSBFishViewController *weakFish = fish;
    surface.startupResultHandler = ^(int error) { [weakFish runtimeStartupFinished:error]; };
    fish.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Fish" image:[UIImage systemImageNamed:@"terminal"] tag:1];
    LSBSettingsViewController *settings = [LSBSettingsViewController new];
    UINavigationController *settingsNavigation = [[UINavigationController alloc] initWithRootViewController:settings];
    settingsNavigation.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Settings" image:[UIImage systemImageNamed:@"gearshape"] tag:2];
    self.viewControllers = @[surface, fish, settingsNavigation];
    // Restoring an explicit opt-in is the only startup path that may activate audio.
    (void)LSBBackgroundAudio.sharedController;
    self.selectedIndex = 0;
    // Install both consumers before asynchronous check-in/output, without focusing the hidden terminal.
    [surface loadViewIfNeeded];
    [fish loadViewIfNeeded];
    if (LSBInitialGuestStartsAutomatically() || LSBInitialGuestStartupWasAttempted()) [surface startInitialRuntime];
}
@end

#import "SettingsViewController.h"
#import "BackgroundAudio.h"
#import "GuestDisplaySettings.h"

@interface LSBHUDSettingsViewController : UITableViewController
@end
@implementation LSBHUDSettingsViewController {
    id _observer;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = @"Performance HUD";
    return self;
}
- (void)dealloc { if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    __weak LSBHUDSettingsViewController *weakSelf = self;
    _observer = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification
        object:LSBGuestDisplaySettings.sharedController queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf.tableView reloadData];
        }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return LSBPerformanceHUDNames().count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return LSBGuestDisplaySettings.sharedController.statusText;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    LSBGuestDisplaySettings *settings = LSBGuestDisplaySettings.sharedController;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.text = [NSString stringWithFormat:@"%ld · %@", (long)indexPath.row, LSBPerformanceHUDNames()[indexPath.row]];
    cell.accessoryType = settings.hudMode == indexPath.row ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.userInteractionEnabled = !settings.pending && !settings.outcomeUnknown && settings.hudMode >= 0;
    cell.textLabel.enabled = cell.userInteractionEnabled;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    [LSBGuestDisplaySettings.sharedController setHUDMode:indexPath.row];
    [self.navigationController popViewControllerAnimated:YES];
}
@end

@implementation LSBSettingsViewController {
    LSBBackgroundAudio *_audio;
    LSBGuestDisplaySettings *_display;
    id _audioObserver, _displayObserver;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = @"Settings";
    return self;
}
- (void)dealloc {
    if (_audioObserver) [NSNotificationCenter.defaultCenter removeObserver:_audioObserver];
    if (_displayObserver) [NSNotificationCenter.defaultCenter removeObserver:_displayObserver];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    _audio = LSBBackgroundAudio.sharedController;
    _display = LSBGuestDisplaySettings.sharedController;
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 54;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
        target:self action:@selector(refreshDisplay)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel = @"Refresh simulator display settings";
    __weak LSBSettingsViewController *weakSelf = self;
    _audioObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBBackgroundAudioDidChangeNotification object:_audio
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf.tableView reloadData];
        }];
    _displayObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBGuestDisplaySettingsDidChangeNotification object:_display
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            LSBSettingsViewController *controller = weakSelf;
            if (!controller) return;
            controller.navigationItem.rightBarButtonItem.enabled = !controller->_display.pending;
            [controller.tableView reloadData];
        }];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDisplay];
}
- (void)refreshDisplay { [_display refresh]; }
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 2; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    return section == 0 ? 1 : 2;
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    return section == 0 ? @"Background execution" : @"Simulator display";
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section == 1) return _display.statusText;
    NSString *description = @"Play silence to keep the host's audio session active in the background. Off by default. "
        "This may increase battery use. Other audio can continue playing.";
    NSString *status = _audio.statusText;
    if (![status isEqualToString:@"Off"] && ![status isEqualToString:@"Playing silence"])
        return [description stringByAppendingFormat:@"\n\n%@", status];
    return description;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    if (indexPath.section == 1 && indexPath.row == 0) {
        cell.textLabel.text = @"Performance HUD";
        cell.detailTextLabel.text = _display.hudMode >= 0 ? LSBPerformanceHUDNames()[_display.hudMode] : @"Unavailable";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.userInteractionEnabled = !_display.pending && !_display.outcomeUnknown && _display.hudMode >= 0;
        cell.textLabel.enabled = cell.userInteractionEnabled;
        return cell;
    }
    BOOL audio = indexPath.section == 0;
    cell.textLabel.text = audio ? @"Silent background audio" : @"Visualize touches";
    UISwitch *toggle = [UISwitch new];
    toggle.on = audio ? _audio.enabled : _display.touches == 1;
    toggle.enabled = audio || (!_display.pending && !_display.outcomeUnknown && _display.touches >= 0);
    cell.textLabel.enabled = toggle.enabled;
    if (!audio && _display.touches < 0) cell.detailTextLabel.text = @"Unavailable";
    toggle.accessibilityLabel = cell.textLabel.text;
    __weak LSBSettingsViewController *weakSelf = self;
    [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
        LSBSettingsViewController *controller = weakSelf;
        if (!controller) return;
        UISwitch *sender = (UISwitch *)action.sender;
        if (audio) [controller->_audio setEnabled:sender.on];
        else {
            BOOL requested = sender.on;
            sender.on = controller->_display.touches == 1; // Show only confirmed readback while the request is pending.
            [controller->_display setVisualizeTouches:requested];
        }
    }] forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 1 && indexPath.row == 0) [self.navigationController pushViewController:[LSBHUDSettingsViewController new] animated:YES];
}
@end

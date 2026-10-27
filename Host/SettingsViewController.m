#import "SettingsViewController.h"
#import "BackgroundAudio.h"

@implementation LSBSettingsViewController {
    LSBBackgroundAudio *_audio;
    id _audioObserver;
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = @"Settings";
    return self;
}
- (void)dealloc {
    if (_audioObserver) [NSNotificationCenter.defaultCenter removeObserver:_audioObserver];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    _audio = LSBBackgroundAudio.sharedController;
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 54;
    __weak LSBSettingsViewController *weakSelf = self;
    _audioObserver = [NSNotificationCenter.defaultCenter addObserverForName:LSBBackgroundAudioDidChangeNotification object:_audio
        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
            (void)notification;
            [weakSelf.tableView reloadData];
        }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return @"Background execution";
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    NSString *description = @"Play silence to keep the host's audio session active in the background. Off by default. "
        "This may increase battery use. Other audio can continue playing.";
    NSString *status = _audio.statusText;
    if (![status isEqualToString:@"Off"] && ![status isEqualToString:@"Playing silence"])
        return [description stringByAppendingFormat:@"\n\n%@", status];
    return description;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.text = @"Silent background audio";
    UISwitch *toggle = [UISwitch new];
    toggle.on = _audio.enabled;
    toggle.accessibilityLabel = cell.textLabel.text;
    __weak LSBBackgroundAudio *weakAudio = _audio;
    [toggle addAction:[UIAction actionWithHandler:^(UIAction *action) {
        UISwitch *sender = (UISwitch *)action.sender;
        [weakAudio setEnabled:sender.on];
    }] forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    return cell;
}
@end

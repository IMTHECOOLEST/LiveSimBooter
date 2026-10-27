#import <Foundation/Foundation.h>
#import "../Host/BackgroundAudioInternal.h"
#include <assert.h>

@interface MemoryAudioDefaults : NSUserDefaults
@property (nonatomic, strong) NSMutableDictionary *values;
@end
@implementation MemoryAudioDefaults
- (instancetype)init { if ((self = [super init])) _values = [NSMutableDictionary new]; return self; }
- (BOOL)boolForKey:(NSString *)key { return [_values[key] boolValue]; }
- (void)setBool:(BOOL)value forKey:(NSString *)key { _values[key] = @(value); }
@end

@interface FakeAudioPlayback : NSObject <LSBBackgroundAudioPlayback>
@property unsigned starts, stops, suspends, resets;
@property BOOL failStart, failStop;
@end
@implementation FakeAudioPlayback
- (BOOL)startWithError:(NSError **)error {
    ++_starts;
    if (_failStart && error) *error = [NSError errorWithDomain:@"test" code:1 userInfo:nil];
    return !_failStart;
}
- (BOOL)stopWithError:(NSError **)error {
    ++_stops;
    if (_failStop && error) *error = [NSError errorWithDomain:@"test" code:2 userInfo:nil];
    return !_failStop;
}
- (void)suspend { ++_suspends; }
- (void)reset { ++_resets; }
@end

static unsigned creations;
static FakeAudioPlayback *lastPlayback;
id<LSBBackgroundAudioPlayback> LSBCreateBackgroundAudioPlayback(LSBBackgroundAudio *controller) {
    assert(controller.enabled);
    ++creations;
    lastPlayback = [FakeAudioPlayback new];
    return lastPlayback;
}

static void defaultOffHasNoAudioEffects(void) {
    MemoryAudioDefaults *defaults = [MemoryAudioDefaults new];
    LSBBackgroundAudio *controller = [[LSBBackgroundAudio alloc] initWithDefaults:defaults playback:nil];
    assert(!controller.enabled && !controller.playing && [controller.statusText isEqualToString:@"Off"]);
    [controller resumeIfEnabled];
    [controller interruptionBegan];
    [controller interruptionEndedShouldResume:YES];
    [controller mediaServicesWereReset];
    [controller setEnabled:NO];
    assert(!creations && defaults.values.count == 1);
    [controller setEnabled:YES];
    assert(creations == 1 && lastPlayback.starts == 1 && controller.enabled && controller.playing);
    [controller resumeIfEnabled];
    assert(lastPlayback.starts == 1);
    [controller setEnabled:NO];
    assert(lastPlayback.stops == 1 && !controller.enabled && !controller.playing);
    [controller resumeIfEnabled];
    assert(lastPlayback.starts == 1);
    LSBBackgroundAudio *reopened = [[LSBBackgroundAudio alloc] initWithDefaults:defaults playback:nil];
    [reopened resumeIfEnabled];
    assert(!reopened.enabled && creations == 1);
}

static void persistentOptInAndInterruptions(void) {
    MemoryAudioDefaults *defaults = [MemoryAudioDefaults new];
    defaults.values[@"LSBSilentBackgroundAudioEnabled"] = @YES;
    FakeAudioPlayback *playback = [FakeAudioPlayback new];
    LSBBackgroundAudio *controller = [[LSBBackgroundAudio alloc] initWithDefaults:defaults playback:playback];
    assert(controller.enabled && !controller.playing && !playback.starts);
    [controller resumeIfEnabled];
    [controller interruptionBegan];
    assert(!controller.playing && controller.enabled && playback.suspends == 1);
    [controller resumeIfEnabled];
    assert(playback.starts == 1); // Foreground notifications cannot override a continuing interruption.
    [controller interruptionEndedShouldResume:NO];
    assert(!controller.playing && playback.starts == 1);
    [controller resumeIfEnabled];
    assert(controller.playing && playback.starts == 2);
    [controller interruptionBegan];
    [controller interruptionEndedShouldResume:YES];
    assert(controller.playing && playback.starts == 3);
    [controller mediaServicesWereReset];
    assert(controller.playing && playback.resets == 1 && playback.starts == 4);
    [controller interruptionBegan];
    [controller setEnabled:NO];
    [controller interruptionEndedShouldResume:YES];
    [controller mediaServicesWereReset];
    [controller resumeIfEnabled];
    assert(!controller.playing && !controller.enabled && playback.starts == 4 && playback.stops == 1);
}

static void failuresStopAndRemainVisible(void) {
    MemoryAudioDefaults *defaults = [MemoryAudioDefaults new];
    FakeAudioPlayback *playback = [FakeAudioPlayback new];
    playback.failStart = YES;
    LSBBackgroundAudio *controller = [[LSBBackgroundAudio alloc] initWithDefaults:defaults playback:playback];
    [controller setEnabled:YES];
    assert(controller.enabled && !controller.playing && playback.starts == 1 && playback.stops == 1);
    assert([controller.statusText containsString:@"Unable to play"]);
    playback.failStart = NO;
    [controller resumeIfEnabled];
    assert(controller.playing && [controller.statusText isEqualToString:@"Playing silence"]);
    [controller playbackFailed:[NSError errorWithDomain:@"decode" code:1 userInfo:nil]];
    assert(!controller.playing && playback.stops == 2 && [controller.statusText containsString:@"playback stopped"]);
    playback.failStop = YES;
    [controller setEnabled:NO];
    assert(!controller.enabled && !controller.playing && [controller.statusText containsString:@"could not deactivate"]);
    unsigned oldStarts = playback.starts;
    [controller resumeIfEnabled];
    [controller interruptionEndedShouldResume:YES];
    assert(playback.starts == oldStarts);
}

int main(void) {
    @autoreleasepool {
        defaultOffHasNoAudioEffects();
        persistentOptInAndInterruptions();
        failuresStopAndRemainVisible();
        puts("Background audio preference/lifecycle tests passed");
    }
    return 0;
}

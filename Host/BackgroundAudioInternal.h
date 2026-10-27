#pragma once

#import "BackgroundAudio.h"

// Keep preference/state decisions testable without opening an audio session.
@protocol LSBBackgroundAudioPlayback <NSObject>
- (BOOL)startWithError:(NSError **)error;
- (BOOL)stopWithError:(NSError **)error;
- (void)suspend;
- (void)reset;
@end

@interface LSBBackgroundAudio (Lifecycle)
- (instancetype)initWithDefaults:(NSUserDefaults *)defaults playback:(id<LSBBackgroundAudioPlayback>)playback;
- (void)resumeIfEnabled;
- (void)interruptionBegan;
- (void)interruptionEndedShouldResume:(BOOL)shouldResume;
- (void)mediaServicesWereReset;
- (void)playbackFailed:(NSError *)error;
@end

id<LSBBackgroundAudioPlayback> LSBCreateBackgroundAudioPlayback(LSBBackgroundAudio *controller);

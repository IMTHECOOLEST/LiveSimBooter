#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Narrow declarations for the native host's FrontBoardServices connection. Verified against iOS 26's framework;
// FBSWorkspaceInitializationOptions replaces the older FBSUIApplicationWorkspace launch delegate API.
@class FBSWorkspace, FBSScene, FBSSceneTransitionContext, FBSWorkspaceCreateSceneResponse, FBSWorkspaceDestroySceneResponse;

typedef void (^FBSWorkspaceCreateSceneCompletion)(FBSWorkspaceCreateSceneResponse * _Nullable response);
typedef void (^FBSWorkspaceDestroySceneCompletion)(FBSWorkspaceDestroySceneResponse * _Nullable response);

@protocol FBSWorkspaceDelegate <NSObject>
@optional
- (void)workspace:(FBSWorkspace *)workspace didCreateScene:(FBSScene *)scene
    withTransitionContext:(FBSSceneTransitionContext *)transition completion:(FBSWorkspaceCreateSceneCompletion)completion;
- (void)workspace:(FBSWorkspace *)workspace willDestroyScene:(FBSScene *)scene
    withTransitionContext:(FBSSceneTransitionContext *)transition completion:(FBSWorkspaceDestroySceneCompletion)completion;
@end

@interface FBSWorkspaceInitializationOptions : NSObject
+ (instancetype)optionsWithDelegate:(id<FBSWorkspaceDelegate>)delegate;
@property(nonatomic, getter=isEndpointMonitoringEnabled) BOOL endpointMonitoringEnabled;
@property(nonatomic, getter=isDefaultShellEndpointEnabled) BOOL defaultShellEndpointEnabled;
@end

@interface FBSWorkspace : NSObject
@end

FOUNDATION_EXPORT FBSWorkspace *FBSWorkspaceInitialize(FBSWorkspaceInitializationOptions *options);

NS_ASSUME_NONNULL_END

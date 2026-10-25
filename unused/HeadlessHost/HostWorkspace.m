#import "HostWorkspace.h"
#import "Private/FrontBoard.h"

#include <stdio.h>

@interface LSBHostWorkspaceDelegate : NSObject <FBSWorkspaceDelegate>
@end

@implementation LSBHostWorkspaceDelegate

- (void)workspace:(FBSWorkspace *)workspace didCreateScene:(FBSScene *)scene
    withTransitionContext:(FBSSceneTransitionContext *)transition completion:(FBSWorkspaceCreateSceneCompletion)completion {
    (void)workspace;
    (void)scene;
    (void)transition;
    // The broker has no UI to prepare. FrontBoard still needs its scene transaction acknowledged.
    fprintf(stderr, "LiveSimBooter: acknowledged headless host scene creation\n");
    if (completion) completion(nil);
}

- (void)workspace:(FBSWorkspace *)workspace willDestroyScene:(FBSScene *)scene
    withTransitionContext:(FBSSceneTransitionContext *)transition completion:(FBSWorkspaceDestroySceneCompletion)completion {
    (void)workspace;
    (void)scene;
    (void)transition;
    if (completion) completion(nil);
}

@end

bool LSBInitializeHostWorkspace(void) {
    if (![NSThread isMainThread]) return false;
    static dispatch_once_t once;
    static LSBHostWorkspaceDelegate *delegate;
    static FBSWorkspace *workspace;
    dispatch_once(&once, ^{
        delegate = [LSBHostWorkspaceDelegate new];
        FBSWorkspaceInitializationOptions *options = [FBSWorkspaceInitializationOptions optionsWithDelegate:delegate];
        options.endpointMonitoringEnabled = YES;
        options.defaultShellEndpointEnabled = YES;
        // FrontBoardServices owns its callback queue. Keep the workspace and delegate alive without creating any UI.
        workspace = FBSWorkspaceInitialize(options);
        fprintf(stderr, "LiveSimBooter: headless FrontBoard workspace %s\n", workspace ? "initialized" : "unavailable");
    });
    return workspace != nil;
}

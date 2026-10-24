//
//  main.m
//  LiveSimBooter
//
//  Created by Duy Tran on 22/10/26.
//

#include "DyldSimLoader.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #import <UIKit/UIKit.h>
// #import "AppDelegate.h"

static int probeDlopen(const char *path) {
    // This diagnostic runs entirely in the host runtime, before mapping or cache detachment.
    fprintf(stderr, "[dlopen probe] path=%s flags=RTLD_NOW|RTLD_LOCAL\n", path);
    (void)dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const char *error = dlerror();
    fprintf(stderr, "[dlopen probe] handle=%p dlerror=%s\n", handle, error ? error : "(none)");
    if (handle) {
        int result = dlclose(handle);
        error = dlerror();
        fprintf(stderr, "[dlopen probe] dlclose=%d dlerror=%s\n", result, error ? error : "(none)");
    }
    fflush(stderr);
    return handle ? 0 : 1;
}

int main(int argc, const char *argv[], const char *envp[], const char *apple[]) {
    // Keep startup in C so Foundation does not start host-runtime work before the cache is removed.
    // NSString *appDelegateClassName = NSStringFromClass([AppDelegate class]);
    // return UIApplicationMain(argc, (char **)argv, nil, appDelegateClassName);

    const char *home = getenv("HOME");
    if (!home || !*home) { fprintf(stderr, "Cannot locate app home directory\n"); return 1; }
    char rootPath[PATH_MAX], dyldPath[PATH_MAX], executablePath[PATH_MAX];
    int rootSize = snprintf(rootPath, sizeof(rootPath), "%s/Documents/RuntimeRoot", home);
    int dyldSize = snprintf(dyldPath, sizeof(dyldPath), "%s/usr/lib/dyld_sim", rootPath);
    int executableSize = snprintf(executablePath, sizeof(executablePath), "%s/bin/df", rootPath);
    if (rootSize < 0 || (size_t)rootSize >= sizeof(rootPath) || dyldSize < 0 || (size_t)dyldSize >= sizeof(dyldPath) ||
        executableSize < 0 || (size_t)executableSize >= sizeof(executablePath)) {
        fprintf(stderr, "Runtime path is too long\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--probe-dlopen-dyld-sim") == 0) return probeDlopen(dyldPath);
    static LSBMappedImage dyldSim, executable;
    char error[1024];
    fprintf(stderr, "Loading %s\n", dyldPath);
    if (!LSBMapDyldSim(dyldPath, &dyldSim, error, sizeof(error))) {
        fprintf(stderr, "dyld_sim load failed: %s\n", error);
        return 1;
    }
    fprintf(stderr, "Loading %s\n", executablePath);
    if (!LSBMapSimExecutable(executablePath, &executable, error, sizeof(error))) {
        fprintf(stderr, "df load failed: %s\n", error);
        LSBUnloadImage(&dyldSim);
        return 1;
    }
    const char *dfArgv[] = {executablePath, NULL};
    // Successful handoff removes the host cache and never returns to host cleanup.
    return LSBEnterDyldSim(&dyldSim, executable.header, executablePath, dyldPath, rootPath, 1, dfArgv, envp, apple);
}

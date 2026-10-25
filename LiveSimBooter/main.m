#include "HostApplication.h"

int main(int argc, const char *argv[], const char *envp[], const char *apple[]) {
    (void)apple;
    return LSBRunHostApplication(argc, argv, envp);
}

#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char script[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", script, sizeof(script) - 1);
    if (length < 0) {
        perror("/proc/self/exe");
        return 1;
    }

    script[length] = '\0';
    char *name = strrchr(script, '/');
    if (length == (ssize_t)(sizeof(script) - 1) || name == NULL ||
        sizeof(script) - (size_t)(name + 1 - script) < sizeof("start")) {
        fputs("Cannot locate the Gizlet Browser startup script.\n", stderr);
        return 1;
    }

    memcpy(name + 1, "start", sizeof("start"));
    if (argc > 0) {
        argv[0] = script;
    }
    execv(script, argv);
    perror(script);
    return 1;
}

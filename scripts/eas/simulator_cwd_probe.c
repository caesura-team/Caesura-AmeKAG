/* Checks simctl's actual working directory before the native test suite runs. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char actual[PATH_MAX];
    if (argc != 2 || getcwd(actual, sizeof(actual)) == NULL) return 2;
    printf("simulator working directory: %s\n", actual);
    if (strcmp(actual, argv[1]) != 0) {
        fprintf(stderr, "Expected native test fixture directory: %s\n", argv[1]);
        return 3;
    }
    if (access("assets", R_OK) || access("scripts", R_OK) || access("demo", R_OK) || access("tests/audio", R_OK)) {
        fprintf(stderr, "Native test fixture directories are missing or unreadable\n");
        return 4;
    }
    return 0;
}

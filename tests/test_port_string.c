// Exercise the production diagnostic formatter at every output-buffer boundary.
#include "../src/ConnectionTester.c"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char* text) {
    ++checks;
    failures += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", text);
}

int main(int argc, char** argv) {
    (void)argv;
    const char* expected = "TCP 47984 | TCP 47989 | TCP 48010 | UDP 47998 | UDP 47999 | UDP 48000 | UDP 48010";
    for (int capacity = 0; capacity <= (int)strlen(expected) + 2; ++capacity) {
        unsigned char guarded[128];
        memset(guarded, 0xA5, sizeof(guarded));
        LiStringifyPortFlags(ML_PORT_FLAG_ALL, " | ", (char*)&guarded[1], capacity);
        int fits = guarded[0] == 0xA5;
        for (int i = 1 + capacity; i < (int)sizeof(guarded); ++i) fits &= guarded[i] == 0xA5;
        if (capacity > 0) {
            size_t prefix = strlen(expected) < (size_t)capacity ? strlen(expected) : (size_t)capacity - 1;
            fits &= memcmp(guarded + 1, expected, prefix) == 0 && guarded[1 + prefix] == 0;
        }
        check(fits, "each capacity preserves guard bytes and produces the expected terminated prefix");
    }
    char buffer[128];
    strcpy(buffer, "unchanged");
    LiStringifyPortFlags(0, NULL, buffer, 0);
    check(strcmp(buffer, "unchanged") == 0, "zero flags still leave a zero-capacity buffer untouched");
    LiStringifyPortFlags(ML_PORT_FLAG_ALL, NULL, buffer, -1);
    check(strcmp(buffer, "unchanged") == 0, "a negative capacity performs no write");
    LiStringifyPortFlags(0, NULL, buffer, sizeof(buffer));
    check(buffer[0] == 0, "zero flags produce an empty string when space exists");
    LiStringifyPortFlags(ML_PORT_FLAG_TCP_47984 | ML_PORT_FLAG_UDP_48000, NULL, buffer, sizeof(buffer));
    check(strcmp(buffer, "TCP 47984UDP 48000") == 0, "a null separator keeps its existing meaning");
    if (argc > 1) {
        LiStringifyPortFlags(ML_PORT_FLAG_ALL, NULL, NULL, 0);
        LiStringifyPortFlags(0, NULL, NULL, -1);
        check(1, "empty capacities do not dereference a null destination");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

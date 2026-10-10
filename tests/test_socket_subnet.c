// Exercise the production private-network classification used to decide
// local vs. remote streaming parameters (packet size, bitrate reserve, QoS).
#include "../src/Limelight-internal.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

static bool classify(const char* text) {
    struct sockaddr_storage address;
    memset(&address, 0, sizeof(address));
    if (strchr(text, ':') != NULL) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&address;
        sin6->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, text, &sin6->sin6_addr) != 1) { printf("FAIL inet_pton %s\n", text); ++failures; return false; }
    }
    else {
        struct sockaddr_in* sin = (struct sockaddr_in*)&address;
        sin->sin_family = AF_INET;
        if (inet_pton(AF_INET, text, &sin->sin_addr) != 1) { printf("FAIL inet_pton %s\n", text); ++failures; return false; }
    }
    return isPrivateNetworkAddress(&address);
}

int main(void) {
    check(classify("fd12:3456::10"), "fd00::/8 unique local (the form routers assign) is private");
    check(classify("fdff:ffff::1"), "fdff:: unique local is private");
    check(classify("fc00::1"), "fc00::/8 unique local is private");
    check(classify("fe80::1"), "fe80::/10 link local is private");
    check(classify("febf::1"), "febf:: (top of fe80::/10) is private");
    check(classify("fec0::1"), "fec0::/10 site local is private");
    check(!classify("7c00::1"), "7c00:: (low seven bits of fc) is not private");
    check(!classify("fe00::1"), "fe00:: is not private");
    check(!classify("fcff:ffff::1") == false, "fcff:: unique local is private");
    check(!classify("2001:db8::1"), "2001:db8:: is not private");
    check(!classify("::ffff:192.168.1.5"), "an IPv4-mapped address is not classified by the IPv6 prefixes");
    check(classify("192.168.1.5"), "192.168.0.0/16 is private");
    check(classify("10.0.0.1"), "10.0.0.0/8 is private");
    check(!classify("8.8.8.8"), "8.8.8.8 is not private");
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

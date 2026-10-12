// STUN attributes are padded to 32 bits (RFC 5389 section 15): the walk towards
// XOR-MAPPED-ADDRESS must step over the padding of an odd-length SOFTWARE or
// ERROR-CODE attribute, or a compliant reply is misparsed (C25-01). The walk is
// exercised on whole binding responses so that either use of the stride, or the
// bound ahead of it, cannot regress silently (C26-02).
#include "../src/Limelight-internal.h"
#include "../src/SimpleStun.c"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

static unsigned char msg[1024];
static int used;
static PSTUN_MESSAGE hdr = (PSTUN_MESSAGE)msg;

static void reset(void) {
    memset(msg, 0, sizeof(msg));
    hdr->messageType = htons(STUN_MESSAGE_BINDING_SUCCESS);
    hdr->magicCookie = htonl(STUN_MESSAGE_COOKIE);
    used = (int)sizeof(STUN_MESSAGE);
}
// Appends an attribute; the value is copied verbatim and the stream is padded to a word
// unless `pad` is false (a lenient sender that omits the padding of its last attribute).
static void attr(unsigned short type, const void* value, int length, int pad) {
    STUN_ATTRIBUTE_HEADER h;
    h.type = htons(type);
    h.length = htons((unsigned short)length);
    memcpy(msg + used, &h, sizeof(h));
    memcpy(msg + used + sizeof(h), value, (size_t)length);
    used += (int)sizeof(h) + (pad ? ((length + 3) & ~3) : length);
}
static void xma(unsigned int address, unsigned char family) {
    unsigned char v[8] = {0, family, 0x12, 0x34, 0, 0, 0, 0};
    unsigned int x = address ^ htonl(STUN_MESSAGE_COOKIE);
    memcpy(v + 4, &x, 4);
    attr(STUN_ATTRIBUTE_XOR_MAPPED_ADDRESS, v, 8, 1);
}
static int walk(unsigned int* out) {
    *out = 0;
    return stunFindXorMappedAddress(hdr, used - (int)sizeof(STUN_MESSAGE), out);
}

int main(void) {
    const unsigned int addr = htonl(0xC0A80102u);
    unsigned int out;

    check(stunAttributeStride(0) == 4, "an empty attribute is its header");
    check(stunAttributeStride(1) == 8 && stunAttributeStride(2) == 8 && stunAttributeStride(3) == 8,
          "1..3 bytes of value are padded to one word");
    check(stunAttributeStride(4) == 8, "a whole word needs no padding");
    check(stunAttributeStride(5) == 12 && stunAttributeStride(8) == 12 && stunAttributeStride(9) == 16,
          "longer values round up to the next word");
    check(stunAttributeStride(65535) == 4 + 65536, "the maximum length does not overflow the int stride");

    reset(); xma(addr, 1);
    check(walk(&out) == 0 && out == addr, "a lone XOR-MAPPED-ADDRESS is decoded");

    reset(); attr(0x8022, "stund", 5, 1); xma(addr, 1);
    check(walk(&out) == 0 && out == addr, "a padded odd-length SOFTWARE ahead of the address is stepped over");

    reset(); attr(0x8022, "coturn-4.6", 10, 1); attr(0x0009, "\0\0\4\1err", 7, 1); xma(addr, 1);
    check(walk(&out) == 0 && out == addr, "two padded attributes ahead of the address are stepped over");

    reset(); xma(addr, 1); attr(0x8022, "stund", 5, 0);
    check(walk(&out) == 0 && out == addr, "an unpadded trailing attribute after the address does not matter");

    reset(); attr(0x8022, "stund", 5, 0);
    check(walk(&out) == -6, "an unpadded trailing attribute without the address is reported as missing, not malformed");

    reset(); attr(0x8022, "stund", 5, 1);
    check(walk(&out) == -6, "a response without the address is reported as missing");

    reset(); attr(0x8022, "stund", 5, 1); xma(addr, 1); used -= 2;
    check(walk(&out) == -5, "a truncated address attribute is malformed");

    reset(); attr(0x8020, "\0\1\x12\x34\0\0", 6, 1);
    check(walk(&out) == -5, "an address attribute of the wrong length is malformed");

    reset(); xma(addr, 2);
    check(walk(&out) == -5, "an IPv6 mapped address is malformed for this IPv4 lookup");

    reset(); attr(0x8022, "stund", 5, 1); attr(0x8020, msg, 0, 1); xma(addr, 1);
    check(walk(&out) == -5, "the comprehension-optional bit does not hide an address attribute from the length check");

    reset();
    check(walk(&out) == -6, "an empty attribute list is reported as missing");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

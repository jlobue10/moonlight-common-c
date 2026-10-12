// STUN attributes are padded to 32 bits (RFC 5389 section 15): the walk towards
// XOR-MAPPED-ADDRESS must step over the padding of an odd-length SOFTWARE or
// ERROR-CODE attribute, or a compliant reply is misparsed (C25-01).
#include "../src/Limelight-internal.h"
#include "../src/SimpleStun.c"
#include <stdio.h>

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

int main(void) {
    check(stunAttributeStride(0) == 4, "an empty attribute is its header");
    check(stunAttributeStride(1) == 8 && stunAttributeStride(2) == 8 && stunAttributeStride(3) == 8,
          "1..3 bytes of value are padded to one word");
    check(stunAttributeStride(4) == 8, "a whole word needs no padding");
    check(stunAttributeStride(5) == 12 && stunAttributeStride(8) == 12 && stunAttributeStride(9) == 16,
          "longer values round up to the next word");
    check(stunAttributeStride(65535) == 4 + 65536, "the maximum length does not overflow the int stride");
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

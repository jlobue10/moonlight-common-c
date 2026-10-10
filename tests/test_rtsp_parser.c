// Exercise the production RTSP response parser with duplicate headers: the later
// value wins and no node is leaked (LeakSanitizer in the sanitized job catches it).
#include "../src/Limelight-internal.h"
#include "../src/Rtsp.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char* text) { ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; }

int main(void) {
    static const char text[] =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 2\r\n"
        "Session: DEADBEEF\r\n"
        "Session: CAFEBABE;timeout = 90\r\n"
        "Session: FEEDFACE\r\n"
        "Transport: RTP/AVP/UDP;unicast;client_port=50000\r\n"
        "CSeq: 2\r\n"
        "\r\n";
    for (int round = 0; round < 50; round++) {
        RTSP_MESSAGE message;
        char* buffer = strdup(text);
        int result = parseRtspMessage(&message, buffer, (int)strlen(buffer));
        if (round == 0) {
            check(result == RTSP_ERROR_SUCCESS, "a response with repeated headers parses");
            const char* session = getOptionContent(message.options, "Session");
            check(session != NULL && strcmp(session, "FEEDFACE") == 0, "the last repeated header value wins");
            int count = 0;
            for (POPTION_ITEM item = message.options; item != NULL; item = item->next) ++count;
            check(count == 3, "repeated headers occupy one node each");
            check(strcmp(getOptionContent(message.options, "CSeq"), "2") == 0 &&
                  strcmp(getOptionContent(message.options, "Transport"), "RTP/AVP/UDP;unicast;client_port=50000") == 0,
                  "the other headers are intact");
        }
        freeMessage(&message);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

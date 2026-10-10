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
        free(buffer);  // the options point into it; freeMessage() frees only the nodes
    }
    {
        // A status outside 100..999 (including atoi's 0 for garbage) is malformed,
        // not a success code the handshake would return as 0.
        static const char* bad[] = {"RTSP/1.0 0 OK\r\nCSeq: 2\r\n\r\n", "RTSP/1.0 abc OK\r\nCSeq: 2\r\n\r\n",
                                    "RTSP/1.0 1000 OK\r\nCSeq: 2\r\n\r\n"};
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            RTSP_MESSAGE message;
            char* buffer = strdup(bad[i]);
            int result = parseRtspMessage(&message, buffer, (int)strlen(buffer));
            check(result == RTSP_ERROR_MALFORMED, "a response with an out-of-range status code is malformed");
            if (result == RTSP_ERROR_SUCCESS) freeMessage(&message);
            free(buffer);
        }
        RTSP_MESSAGE message;
        char* buffer = strdup("RTSP/1.0 404 Not Found\r\nCSeq: 3\r\n\r\n");
        check(parseRtspMessage(&message, buffer, (int)strlen(buffer)) == RTSP_ERROR_SUCCESS &&
                  message.message.response.statusCode == 404,
              "an in-range error status still parses");
        freeMessage(&message);
        free(buffer);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

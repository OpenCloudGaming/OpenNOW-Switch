#include "sctp.h"
#include "sdp.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char sdp[CONFIG_SDP_BUFFER_SIZE] = {0};
    sdp_create(sdp, 1, 1, 1);
    sdp_append_datachannel(sdp);
    const char* attribute = strstr(sdp, "a=max-message-size:");
    assert(attribute != NULL);
    const unsigned long advertised = strtoul(attribute + strlen("a=max-message-size:"), NULL, 10);
    assert(advertised == SCTP_MAX_MESSAGE_SIZE);
    assert(advertised <= SCTP_RECEIVE_BUFFER_SIZE);
    return 0;
}

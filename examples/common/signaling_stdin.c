/*
 * nanortc examples — stdin/stdout signaling
 *
 * Simplest possible signaling for debugging:
 *   1. User pastes remote SDP offer into stdin (terminated by empty line)
 *   2. Local SDP answer is printed to stdout
 *   3. User copies answer to the remote peer (e.g., browser console)
 *
 * SPDX-License-Identifier: MIT
 */

#include "signaling.h"

#include <stdio.h>
#include <string.h>

int nano_signaling_init(nano_signaling_t *sig, nano_signaling_type_t type)
{
    if (!sig) {
        return -1;
    }
    memset(sig, 0, sizeof(*sig));
    sig->type = type;
    return 0;
}

int nano_signaling_recv_offer(nano_signaling_t *sig, char *buf, size_t buf_len)
{
    (void)sig;

    if (!buf || buf_len == 0) {
        return -1;
    }

    fprintf(stderr, "\n--- Paste remote SDP offer (end with empty line) ---\n");

    size_t total = 0;
    char line[512];

    while (fgets(line, sizeof(line), stdin)) {
        /* Empty line (just newline) terminates input */
        if (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n')) {
            break;
        }

        size_t len = strlen(line);
        if (total + len >= buf_len - 1) {
            fprintf(stderr, "Error: SDP buffer overflow\n");
            return -1;
        }

        memcpy(buf + total, line, len);
        total += len;
    }

    buf[total] = '\0';

    if (total == 0) {
        fprintf(stderr, "Error: empty SDP offer\n");
        return -1;
    }

    fprintf(stderr, "--- Received %zu bytes of SDP ---\n\n", total);
    return (int)total;
}

int nano_signaling_send_answer(nano_signaling_t *sig, const char *answer)
{
    (void)sig;

    if (!answer) {
        return -1;
    }

    fprintf(stderr, "\n--- Local SDP answer (copy to remote peer) ---\n");
    printf("%s\n", answer);
    fprintf(stderr, "--- End of SDP answer ---\n\n");

    return 0;
}

void nano_signaling_destroy(nano_signaling_t *sig)
{
    (void)sig;
}

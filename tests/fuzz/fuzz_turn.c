/*
 * Fuzz harness for TURN — nano_turn.c
 *
 * Targets: turn_unwrap_data(), turn_unwrap_channel_data(), turn_handle_response()
 * Attack surface: TURN server responses and relayed data from network.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_turn.h"
#include "nanortc.h"
#include "nanortc_crypto.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Minimal crypto provider stub for fuzzing (HMAC-SHA1 is a no-op) */
static void fuzz_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                           uint8_t out[20])
{
    (void)key;
    (void)key_len;
    (void)data;
    (void)data_len;
    memset(out, 0, 20);
}

static void fuzz_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    (void)data;
    (void)len;
    memset(out, 0, 16);
}

static int fuzz_random(uint8_t *buf, size_t len)
{
    memset(buf, 0x42, len);
    return 0;
}

static const nanortc_crypto_provider_t fuzz_crypto = {
    .hmac_sha1 = fuzz_hmac_sha1,
    .md5 = fuzz_md5,
    .random_bytes = fuzz_random,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Exercise Data indication unwrap (arbitrary input) */
    {
        uint8_t peer_addr[NANORTC_ADDR_SIZE];
        uint8_t peer_family = 0;
        uint16_t peer_port = 0;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        turn_unwrap_data(data, size, peer_addr, &peer_family, &peer_port, &payload, &payload_len);
    }

    /* Exercise ChannelData unwrap (arbitrary input) */
    {
        uint16_t channel = 0;
        const uint8_t *cd_payload = NULL;
        size_t cd_len = 0;
        turn_unwrap_channel_data(data, size, &channel, &cd_payload, &cd_len);
    }

    /* Exercise ChannelData detection */
    turn_is_channel_data(data, size);

    /* Exercise response handling with pre-configured ALLOCATED state */
    {
        nano_turn_t turn;
        turn_init(&turn);
        turn.state = NANORTC_TURN_ALLOCATED;
        turn.configured = true;
        turn.hmac_key_valid = true;
        memset(turn.auth.hmac_key, 0xAA, NANORTC_TURN_HMAC_KEY_SIZE);

        /* Copy transaction ID from fuzzer input so responses may match */
        if (size >= STUN_HEADER_SIZE) {
            uint16_t method = nanortc_read_u16be(data) & 0x000fu;
            if (method == 8) {
                turn.permission_count = 1;
                turn.permissions[0].family = 4;
                turn.permissions[0].pending = NANORTC_TURN_REQUEST_WAITING;
                turn.permissions[0].transmissions = 1;
                memcpy(turn.permissions[0].txid, data + 8, STUN_TXID_SIZE);
            } else if (method == 9) {
                turn.channel_count = 1;
                turn.channels[0].family = 4;
                turn.channels[0].channel = 0x4000;
                turn.channels[0].pending = NANORTC_TURN_REQUEST_WAITING;
                turn.channels[0].transmissions = 1;
                memcpy(turn.channels[0].txid, data + 8, STUN_TXID_SIZE);
            } else {
                turn.transaction =
                    method == 3 ? NANORTC_TURN_TXN_ALLOCATE : NANORTC_TURN_TXN_REFRESH;
                turn.transaction_authenticated = true;
                turn.transaction_transmissions = 1;
                if (method == 3)
                    turn.state = NANORTC_TURN_ALLOCATING;
                memcpy(turn.last_txid, data + 8, STUN_TXID_SIZE);
            }
        }

        uint8_t packet[NANORTC_TURN_MAX_REQUEST_SIZE];
        uint32_t now = UINT32_MAX - 100u;
        for (unsigned i = 0; i < 4; i++) {
            turn_handle_response(&turn, now, data, size, &fuzz_crypto);
            size_t len;
            turn_poll_output(&turn, now, &fuzz_crypto, packet, sizeof(packet), &len);
            uint32_t delay = turn_next_timeout_ms(&turn, now);
            if (delay == UINT32_MAX)
                break;
            now += delay;
        }
    }

    return 0;
}

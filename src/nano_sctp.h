/*
 * nanortc — SCTP-Lite internal interface (RFC 4960, RFC 3758)
 *
 * Minimal SCTP for WebRTC DataChannel over DTLS.
 * Reference: libpeer sctp.h (struct layout), str0m sctp/mod.rs (Sans I/O).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SCTP_H_
#define NANO_SCTP_H_

#include "nanortc_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward-declare crypto provider */
#ifndef NANO_CRYPTO_PROVIDER_T_DECLARED
#define NANO_CRYPTO_PROVIDER_T_DECLARED
typedef struct nano_crypto_provider nano_crypto_provider_t;
#endif

/* ----------------------------------------------------------------
 * SCTP chunk types (RFC 4960 §3.2)
 * ---------------------------------------------------------------- */

#define SCTP_CHUNK_DATA          0
#define SCTP_CHUNK_INIT          1
#define SCTP_CHUNK_INIT_ACK      2
#define SCTP_CHUNK_SACK          3
#define SCTP_CHUNK_HEARTBEAT     4
#define SCTP_CHUNK_HEARTBEAT_ACK 5
#define SCTP_CHUNK_ABORT         6
#define SCTP_CHUNK_SHUTDOWN      7
#define SCTP_CHUNK_SHUTDOWN_ACK  8
#define SCTP_CHUNK_ERROR         9
#define SCTP_CHUNK_COOKIE_ECHO   10
#define SCTP_CHUNK_COOKIE_ACK    11
#define SCTP_CHUNK_FORWARD_TSN   192

/* DATA chunk flags (RFC 4960 §6.9) */
#define SCTP_DATA_FLAG_END       0x01 /* E bit: last fragment */
#define SCTP_DATA_FLAG_BEGIN     0x02 /* B bit: first fragment */
#define SCTP_DATA_FLAG_UNORDERED 0x04 /* U bit: unordered delivery */

/* State Cookie parameter type (RFC 4960 §3.3.3) */
#define SCTP_PARAM_STATE_COOKIE 7

/* ----------------------------------------------------------------
 * Parsed chunk structures (for internal codec use)
 * ---------------------------------------------------------------- */

/** Parsed SCTP common header (12 bytes on wire). */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t vtag;
    uint32_t checksum;
} sctp_header_t;

/** Parsed INIT / INIT-ACK body. */
typedef struct {
    uint32_t initiate_tag;
    uint32_t a_rwnd;
    uint16_t num_ostreams;
    uint16_t num_istreams;
    uint32_t initial_tsn;
    const uint8_t *cookie;     /* pointer into packet buffer (INIT-ACK only) */
    uint16_t cookie_len;
} sctp_init_t;

/** Parsed DATA chunk body. */
typedef struct {
    uint32_t tsn;
    uint16_t stream_id;
    uint16_t ssn;
    uint32_t ppid;
    uint8_t flags;             /* B/E/U bits */
    const uint8_t *payload;
    uint16_t payload_len;
} sctp_data_t;

/** Parsed SACK chunk body. */
typedef struct {
    uint32_t cumulative_tsn;
    uint32_t a_rwnd;
    uint16_t num_gap_blocks;
    uint16_t num_dup_tsns;
} sctp_sack_t;

/** Parsed FORWARD-TSN chunk. */
typedef struct {
    uint32_t new_cumulative_tsn;
} sctp_forward_tsn_t;

/* ----------------------------------------------------------------
 * Send queue entry
 * ---------------------------------------------------------------- */

typedef struct {
    uint32_t tsn;
    uint16_t stream_id;
    uint16_t ssn;
    uint32_t ppid;
    uint16_t data_offset;      /* offset into send_buf */
    uint16_t data_len;
    uint32_t sent_at_ms;       /* timestamp of last send (for RTO) */
    uint8_t retransmit_count;
    uint8_t flags;             /* B/E/U bits */
    bool acked;
    bool in_flight;
} sctp_send_entry_t;

/* ----------------------------------------------------------------
 * Association state
 * ---------------------------------------------------------------- */

typedef enum {
    NANO_SCTP_STATE_CLOSED,
    NANO_SCTP_STATE_COOKIE_WAIT,
    NANO_SCTP_STATE_COOKIE_ECHOED,
    NANO_SCTP_STATE_ESTABLISHED,
    NANO_SCTP_STATE_SHUTDOWN_PENDING,
    NANO_SCTP_STATE_SHUTDOWN_SENT,
} nano_sctp_state_t;

/* ----------------------------------------------------------------
 * Main SCTP state structure
 * ---------------------------------------------------------------- */

typedef struct nano_sctp {
    nano_sctp_state_t state;

    /* Association identifiers */
    uint32_t local_vtag;
    uint32_t remote_vtag;
    uint16_t local_port;
    uint16_t remote_port;

    /* TSN counters */
    uint32_t next_tsn;          /* next TSN to assign to outbound DATA */
    uint32_t peer_initial_tsn;

    /* Stream sequence numbers (per outbound stream) */
    uint16_t next_ssn[NANO_MAX_DATACHANNELS];

    /* Receive state */
    uint32_t cumulative_tsn;    /* highest TSN such that all TSN <= this received */
    bool sack_needed;

    /* Send queue */
    sctp_send_entry_t send_queue[NANO_SCTP_MAX_SEND_QUEUE];
    uint8_t sq_head;
    uint8_t sq_tail;
    uint8_t send_buf[NANO_SCTP_SEND_BUF_SIZE];
    uint16_t send_buf_used;

    /* Retransmission */
    uint32_t rto_ms;
    uint32_t last_send_ms;

    /* HEARTBEAT */
    uint32_t last_heartbeat_ms;
    bool heartbeat_pending;
    uint8_t heartbeat_nonce[8];

    /* Handshake cookie storage */
    uint8_t cookie[NANO_SCTP_COOKIE_SIZE];
    uint16_t cookie_len;
    uint8_t cookie_secret[16]; /* HMAC key for cookie generation (server) */

    /* Peer parameters (from INIT/INIT-ACK) */
    uint32_t peer_a_rwnd;
    uint16_t peer_num_istreams;
    uint16_t peer_num_ostreams;

    /* Output buffer (assembled SCTP packet for poll_output) */
    uint8_t out_buf[NANO_SCTP_MTU];
    uint16_t out_len;
    bool has_output;

    /* Delivered message (available to caller after handle_data) */
    const uint8_t *delivered_data;
    uint16_t delivered_len;
    uint16_t delivered_stream;
    uint32_t delivered_ppid;
    bool has_delivered;

    /* Crypto provider (for cookie HMAC + random) */
    const nano_crypto_provider_t *crypto;
} nano_sctp_t;

/* ----------------------------------------------------------------
 * Functions
 * ---------------------------------------------------------------- */

/** Initialize SCTP state. */
int sctp_init(nano_sctp_t *sctp);

/** Feed incoming SCTP packet (after DTLS decrypt). */
int sctp_handle_data(nano_sctp_t *sctp, const uint8_t *data, size_t len);

/** Poll for outgoing SCTP packet (to be DTLS-encrypted). */
int sctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len,
                     size_t *out_len);

/** Enqueue application data for transmission. */
int sctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid,
              const uint8_t *data, size_t len);

/** Initiate SCTP association (client role — sends INIT). */
int sctp_start(nano_sctp_t *sctp);

/** Handle timeout (retransmission, heartbeat). */
int sctp_handle_timeout(nano_sctp_t *sctp, uint32_t now_ms);

/* ----------------------------------------------------------------
 * Codec functions (parse / encode individual chunks)
 * ---------------------------------------------------------------- */

int sctp_parse_header(const uint8_t *data, size_t len, sctp_header_t *hdr);
int sctp_verify_checksum(const uint8_t *data, size_t len);
int sctp_parse_init(const uint8_t *chunk, size_t chunk_len, sctp_init_t *out);
int sctp_parse_data(const uint8_t *chunk, size_t chunk_len, sctp_data_t *out);
int sctp_parse_sack(const uint8_t *chunk, size_t chunk_len, sctp_sack_t *out);

size_t sctp_encode_header(uint8_t *buf, uint16_t src_port, uint16_t dst_port,
                          uint32_t vtag);
void sctp_finalize_checksum(uint8_t *packet, size_t len);

size_t sctp_encode_init(uint8_t *buf, uint8_t type, uint32_t initiate_tag,
                        uint32_t a_rwnd, uint16_t num_ostreams,
                        uint16_t num_istreams, uint32_t initial_tsn,
                        const uint8_t *cookie, uint16_t cookie_len);

size_t sctp_encode_cookie_echo(uint8_t *buf, const uint8_t *cookie,
                               uint16_t cookie_len);
size_t sctp_encode_cookie_ack(uint8_t *buf);
size_t sctp_encode_data(uint8_t *buf, uint32_t tsn, uint16_t stream_id,
                        uint16_t ssn, uint32_t ppid, uint8_t flags,
                        const uint8_t *payload, uint16_t payload_len);
size_t sctp_encode_sack(uint8_t *buf, uint32_t cumulative_tsn, uint32_t a_rwnd);
size_t sctp_encode_heartbeat(uint8_t *buf, const uint8_t *info,
                             uint16_t info_len);
size_t sctp_encode_heartbeat_ack(uint8_t *buf, const uint8_t *info,
                                 uint16_t info_len);
size_t sctp_encode_forward_tsn(uint8_t *buf, uint32_t new_cumulative_tsn);
size_t sctp_encode_shutdown(uint8_t *buf, uint32_t cumulative_tsn);

#endif /* NANO_SCTP_H_ */

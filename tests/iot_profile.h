/*
 * IoT DataChannel profile header
 *
 * Enabled via -DNANORTC_CONFIG_FILE=\"tests/iot_profile.h\".
 * Shrinks SCTP + DTLS buffers by ~8 KB per nanortc_t instance on
 * x86_64 (nano_dtls_t 6400→4864, nano_sctp_t 13784→7288). Intended
 * for DC-only deployments on low-jitter LANs where smaller reassembly
 * budgets are acceptable. Documented in
 * docs/engineering/memory-profiles.md ("IoT DC Profile").
 */
#ifndef NANORTC_IOT_PROFILE_H
#define NANORTC_IOT_PROFILE_H

/* Identity macro — gates tighter sizeof bounds in tests/test_sizeof.c */
#define NANORTC_IOT_PROFILE 1

/* SCTP buffers: 4096 → 2048, out queue 4 → 2 */
#define NANORTC_SCTP_SEND_BUF_SIZE     2048
#define NANORTC_SCTP_RECV_BUF_SIZE     2048
#define NANORTC_SCTP_RECV_GAP_BUF_SIZE 2048
#define NANORTC_SCTP_OUT_QUEUE_SIZE    2

/* DTLS buffers: 2048 → 1536 (three buffers live in nano_dtls_t) */
#define NANORTC_DTLS_BUF_SIZE 1536

#endif /* NANORTC_IOT_PROFILE_H */

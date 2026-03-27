/*
 * nanortc — Main state machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_crypto.h"
#include "nano_ice.h"
#include "nano_stun.h"
#include "nano_sdp.h"
#include "nano_log.h"
#include "nanortc_util.h"

#if NANO_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#include <string.h>

int nano_rtc_init(nano_rtc_t *rtc, const nano_rtc_config_t *cfg)
{
    if (!rtc || !cfg) {
        return NANO_ERR_INVALID_PARAM;
    }

    memset(rtc, 0, sizeof(*rtc));
    rtc->config = *cfg;
    rtc->state = NANO_STATE_NEW;

    /* Initialize logging (process-global callback) */
    nano_log_init(&cfg->log);

    NANO_LOGI("RTC", "nano_rtc_init");

    ice_init(&rtc->ice, cfg->role == NANO_ROLE_CONTROLLING);
    /* DTLS context is created early in accept_offer (for SDP fingerprint);
     * handshake starts when ICE connects. */
    sdp_init(&rtc->sdp);

#if NANO_FEATURE_DATACHANNEL
    nsctp_init(&rtc->sctp);
    dc_init(&rtc->datachannel);
#endif

    /* Default DTLS setup from ICE role (overridden by SDP negotiation in accept_offer) */
    if (cfg->role == NANO_ROLE_CONTROLLING) {
        rtc->sdp.local_setup = NANO_SDP_SETUP_ACTIVE;
    }

#if NANO_HAVE_MEDIA_TRANSPORT
    rtp_init(&rtc->rtp, 0, 0);
    rtcp_init(&rtc->rtcp, 0);
    srtp_init(&rtc->srtp);
#endif

#if NANO_FEATURE_AUDIO
    jitter_init(&rtc->jitter, cfg->jitter_depth_ms);
#endif

#if NANO_FEATURE_VIDEO
    bwe_init(&rtc->bwe);
#endif

    return NANO_OK;
}

void nano_rtc_destroy(nano_rtc_t *rtc)
{
    if (!rtc) {
        return;
    }
    NANO_LOGI("RTC", "nano_rtc_destroy");
    dtls_destroy(&rtc->dtls);
    nano_log_cleanup();
    rtc->state = NANO_STATE_CLOSED;
}

/* Cache DTLS fingerprint with "sha-256 " prefix into SDP state (RFC 8122 §5) */
static void rtc_cache_fingerprint(nano_rtc_t *rtc)
{
    if (rtc->sdp.local_fingerprint[0] != '\0')
        return;
    const char *fp = dtls_get_fingerprint(&rtc->dtls);
    if (!fp)
        return;
    size_t fplen = nano_strnlen(fp, sizeof(rtc->dtls.local_fingerprint));
    if (8 + fplen < sizeof(rtc->sdp.local_fingerprint)) {
        memcpy(rtc->sdp.local_fingerprint, "sha-256 ", 8);
        memcpy(rtc->sdp.local_fingerprint + 8, fp, fplen + 1);
    }
}

int nano_accept_offer(nano_rtc_t *rtc, const char *offer, char *answer_buf, size_t answer_buf_len)
{
    if (!rtc || !offer || !answer_buf) {
        return NANO_ERR_INVALID_PARAM;
    }

    size_t offer_len = strlen(offer); /* NANO_SAFE: API boundary */

    /* Parse remote SDP */
    int rc = sdp_parse(&rtc->sdp, offer, offer_len);
    if (rc != NANO_OK) {
        return rc;
    }

    /* Copy remote ICE credentials to ICE state */
    memcpy(rtc->ice.remote_ufrag, rtc->sdp.remote_ufrag, sizeof(rtc->ice.remote_ufrag));
    memcpy(rtc->ice.remote_pwd, rtc->sdp.remote_pwd, sizeof(rtc->ice.remote_pwd));
    rtc->ice.remote_ufrag_len = strlen(rtc->sdp.remote_ufrag); /* NANO_SAFE: API boundary */
    rtc->ice.remote_pwd_len = strlen(rtc->sdp.remote_pwd);     /* NANO_SAFE: API boundary */

#if NANO_FEATURE_DATACHANNEL
    /* Set SCTP remote port from SDP */
    if (rtc->sdp.remote_sctp_port > 0) {
        rtc->sctp.remote_port = rtc->sdp.remote_sctp_port;
    }

    /* Set crypto provider on SCTP for cookie generation */
    rtc->sctp.crypto = rtc->config.crypto;
#endif

    /* Generate local ICE credentials via crypto random */
    if (rtc->config.crypto) {
        /* Generate ufrag as hex (NANO_ICE_UFRAG_LEN/2 random bytes) */
        uint8_t rnd[NANO_ICE_UFRAG_LEN / 2];
        rtc->config.crypto->random_bytes(rnd, sizeof(rnd));
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < (int)sizeof(rnd); i++) {
            rtc->sdp.local_ufrag[i * 2] = hex[(rnd[i] >> 4) & 0xF];
            rtc->sdp.local_ufrag[i * 2 + 1] = hex[rnd[i] & 0xF];
        }
        rtc->sdp.local_ufrag[NANO_ICE_UFRAG_LEN] = '\0';

        /* Generate pwd as hex (NANO_ICE_PWD_LEN/2 random bytes) */
        uint8_t rnd2[NANO_ICE_PWD_LEN / 2];
        rtc->config.crypto->random_bytes(rnd2, sizeof(rnd2));
        for (int i = 0; i < (int)sizeof(rnd2); i++) {
            rtc->sdp.local_pwd[i * 2] = hex[(rnd2[i] >> 4) & 0xF];
            rtc->sdp.local_pwd[i * 2 + 1] = hex[rnd2[i] & 0xF];
        }
        rtc->sdp.local_pwd[NANO_ICE_PWD_LEN] = '\0';

        /* Copy to ICE state */
        memcpy(rtc->ice.local_ufrag, rtc->sdp.local_ufrag, sizeof(rtc->ice.local_ufrag));
        memcpy(rtc->ice.local_pwd, rtc->sdp.local_pwd, sizeof(rtc->ice.local_pwd));
        rtc->ice.local_ufrag_len = NANO_ICE_UFRAG_LEN;
        rtc->ice.local_pwd_len = NANO_ICE_PWD_LEN;
    }

    /* Determine DTLS role from remote setup (RFC 8842 §5.2) */
    if (rtc->sdp.remote_setup == NANO_SDP_SETUP_ACTIVE) {
        rtc->sdp.local_setup = NANO_SDP_SETUP_PASSIVE;
    } else if (rtc->sdp.remote_setup == NANO_SDP_SETUP_PASSIVE) {
        rtc->sdp.local_setup = NANO_SDP_SETUP_ACTIVE;
    } else {
        /* Remote is actpass (offerer default) — answerer chooses passive */
        rtc->sdp.local_setup = NANO_SDP_SETUP_PASSIVE;
    }

    /* Early DTLS init: create certificate for SDP fingerprint (RFC 8827 §5) */
    if (rtc->config.crypto && !rtc->dtls.crypto_ctx) {
        int is_dtls_server = (rtc->sdp.local_setup == NANO_SDP_SETUP_PASSIVE);
        int drc = dtls_init(&rtc->dtls, rtc->config.crypto, is_dtls_server);
        if (drc != NANO_OK) {
            return drc;
        }
        rtc_cache_fingerprint(rtc);
    }

    /* Generate answer SDP */
    size_t answer_len = 0;
    rc = sdp_generate_answer(&rtc->sdp, answer_buf, answer_buf_len, &answer_len);
    if (rc != NANO_OK) {
        return rc;
    }

    NANO_LOGI("RTC", "offer accepted, answer generated");
    return (int)answer_len;
}

int nano_create_offer(nano_rtc_t *rtc, char *offer_buf, size_t offer_buf_len)
{
    (void)rtc;
    (void)offer_buf;
    (void)offer_buf_len;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_accept_answer(nano_rtc_t *rtc, const char *answer)
{
    (void)rtc;
    (void)answer;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_add_local_candidate(nano_rtc_t *rtc, const char *ip, uint16_t port)
{
    if (!rtc || !ip) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* Store in SDP state for answer generation (a=candidate: line) */
    size_t ip_len = strlen(ip); /* NANO_SAFE: API boundary */
    if (ip_len >= sizeof(rtc->sdp.local_candidate_ip)) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(rtc->sdp.local_candidate_ip, ip, ip_len + 1);
    rtc->sdp.local_candidate_port = port;
    rtc->sdp.has_local_candidate = true;

    NANO_LOGI("RTC", "local candidate added");
    return NANO_OK;
}

int nano_add_remote_candidate(nano_rtc_t *rtc, const char *candidate_str)
{
    if (!rtc || !candidate_str) {
        return NANO_ERR_INVALID_PARAM;
    }

    /*
     * Parse SDP candidate attribute (RFC 8839 §5.1):
     *   "candidate:<foundation> <component> UDP <priority> <addr> <port> typ <type>"
     * We need fields 5 (addr) and 6 (port), 1-indexed from "candidate:".
     * Also accept plain "<addr> <port>" for simple use cases.
     */
    const char *p = candidate_str;

    /* Skip "candidate:" prefix if present */
    const char *prefix = "candidate:";
    size_t pfx_len = 10;
    bool has_prefix = true;
    for (size_t i = 0; i < pfx_len; i++) {
        if (p[i] == '\0' || p[i] != prefix[i]) {
            has_prefix = false;
            break;
        }
    }

    const char *addr_str = NULL;
    size_t addr_len = 0;
    uint16_t port = 0;

    if (has_prefix) {
        /* SDP format: skip to field 5 (addr) and field 6 (port) */
        p += pfx_len;
        int field = 1;
        while (*p && field < 5) {
            if (*p == ' ') {
                field++;
                while (*p == ' ')
                    p++;
            } else {
                p++;
            }
        }
        /* p now points to addr field */
        addr_str = p;
        while (*p && *p != ' ')
            p++;
        addr_len = (size_t)(p - addr_str);

        /* Skip to port field */
        while (*p == ' ')
            p++;
        /* Parse port */
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (uint16_t)(*p - '0');
            p++;
        }
    } else {
        /* Simple format: "<addr> <port>" */
        addr_str = candidate_str;
        while (*p && *p != ' ')
            p++;
        addr_len = (size_t)(p - addr_str);
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (uint16_t)(*p - '0');
            p++;
        }
    }

    if (addr_len == 0 || addr_len >= NANO_IPV6_STR_SIZE || port == 0) {
        return NANO_ERR_PARSE;
    }

    /* Parse IPv4 address (simple: a.b.c.d) */
    char addr_buf[NANO_IPV6_STR_SIZE];
    memcpy(addr_buf, addr_str, addr_len);
    addr_buf[addr_len] = '\0';

    uint8_t ip[4] = {0};
    int octet = 0;
    uint16_t val = 0;
    for (size_t i = 0; i <= addr_len; i++) {
        if (i == addr_len || addr_buf[i] == '.') {
            if (octet >= 4 || val > 255) {
                return NANO_ERR_PARSE;
            }
            ip[octet++] = (uint8_t)val;
            val = 0;
        } else if (addr_buf[i] >= '0' && addr_buf[i] <= '9') {
            val = val * 10 + (uint16_t)(addr_buf[i] - '0');
        } else {
            /* Not a simple IPv4 — could be IPv6, not yet supported */
            return NANO_ERR_NOT_IMPLEMENTED;
        }
    }

    if (octet != 4) {
        return NANO_ERR_PARSE;
    }

    /* Store in ICE state */
    rtc->ice.remote_family = 4;
    memset(rtc->ice.remote_addr, 0, NANO_ADDR_SIZE);
    memcpy(rtc->ice.remote_addr, ip, 4);
    rtc->ice.remote_port = port;

    NANO_LOGI("RTC", "remote candidate added");
    return NANO_OK;
}

int nano_poll_output(nano_rtc_t *rtc, nano_output_t *out)
{
    if (!rtc || !out) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (rtc->out_head == rtc->out_tail) {
        return NANO_ERR_NO_DATA;
    }
    *out = rtc->out_queue[rtc->out_head & (NANO_OUT_QUEUE_SIZE - 1)];
    rtc->out_head++;
    return NANO_OK;
}

/* Init DTLS (if needed) and begin handshake after ICE connects */
static int rtc_begin_dtls_handshake(nano_rtc_t *rtc, const nano_addr_t *src)
{
    int is_server = (rtc->sdp.local_setup == NANO_SDP_SETUP_PASSIVE);

    /* accept_offer() does early init; this guard covers create_offer() path */
    if (!rtc->dtls.crypto_ctx) {
        int rc = dtls_init(&rtc->dtls, rtc->config.crypto, is_server);
        if (rc != NANO_OK)
            return rc;
    }

    rtc->state = NANO_STATE_DTLS_HANDSHAKING;

    if (!is_server) {
        int rc = dtls_start(&rtc->dtls);
        if (rc != NANO_OK)
            return rc;
        size_t dout_len = 0;
        if (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch), &dout_len) ==
                NANO_OK &&
            dout_len > 0) {
            nano_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANO_OUTPUT_TRANSMIT;
            tout.transmit.data = rtc->dtls_scratch;
            tout.transmit.len = dout_len;
            tout.transmit.dest = *src;
            rtc_enqueue_output(rtc, &tout);
        }
    }
    return NANO_OK;
}

#if NANO_FEATURE_DATACHANNEL
/* ----------------------------------------------------------------
 * Internal: drain SCTP output through DTLS encrypt → transmit queue
 * ---------------------------------------------------------------- */

static void rtc_pump_sctp_through_dtls(nano_rtc_t *rtc, const nano_addr_t *dest)
{
    size_t nsctp_out = 0;
    uint8_t nsctp_buf[NANO_SCTP_MTU];
    while (nsctp_poll_output(&rtc->sctp, nsctp_buf, sizeof(nsctp_buf), &nsctp_out) == NANO_OK &&
           nsctp_out > 0) {
        dtls_encrypt(&rtc->dtls, nsctp_buf, nsctp_out);
        size_t enc_len = 0;
        while (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch),
                                &enc_len) == NANO_OK &&
               enc_len > 0) {
            nano_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANO_OUTPUT_TRANSMIT;
            tout.transmit.data = rtc->dtls_scratch;
            tout.transmit.len = enc_len;
            tout.transmit.dest = *dest;
            rtc_enqueue_output(rtc, &tout);
            enc_len = 0;
        }
        nsctp_out = 0;
    }
}
#endif /* NANO_FEATURE_DATACHANNEL */

/* ----------------------------------------------------------------
 * nano_handle_receive — RFC 7983 demux
 * ---------------------------------------------------------------- */

int nano_handle_receive(nano_rtc_t *rtc, uint32_t now_ms, const uint8_t *data, size_t len,
                        const nano_addr_t *src)
{
    if (!rtc || !data || len == 0 || !src) {
        return NANO_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;
    uint8_t first = data[0];

    /* RFC 7983 §3: demultiplexing by first byte */
    if (first <= 3) {
        /* STUN [0x00-0x03] */
        size_t resp_len = 0;
        int rc = ice_handle_stun(&rtc->ice, data, len, src, rtc->config.crypto, rtc->stun_buf,
                                 sizeof(rtc->stun_buf), &resp_len);
        if (rc != NANO_OK) {
            return rc;
        }

        /* Enqueue STUN response for transmission */
        if (resp_len > 0) {
            nano_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANO_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = resp_len;
            out.transmit.dest = *src; /* reply to sender */
            rtc_enqueue_output(rtc, &out);
        }

        /* Check for ICE state transition → init DTLS + emit event */
        if (rtc->ice.state == NANO_ICE_STATE_CONNECTED && rtc->state < NANO_STATE_ICE_CONNECTED) {
            rtc->state = NANO_STATE_ICE_CONNECTED;

            nano_output_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = NANO_OUTPUT_EVENT;
            evt.event.type = NANO_EVENT_ICE_CONNECTED;
            rtc_enqueue_output(rtc, &evt);

            int drc = rtc_begin_dtls_handshake(rtc, src);
            if (drc != NANO_OK) {
                return drc;
            }
        }

        return NANO_OK;

    } else if (first >= 20 && first <= 63) {
        /* DTLS [0x14-0x3F] — RFC 6347 */
        if (rtc->state < NANO_STATE_ICE_CONNECTED) {
            return NANO_ERR_STATE; /* ICE must complete first */
        }

        int drc = dtls_handle_data(&rtc->dtls, data, len);
        if (drc < 0) {
            return drc;
        }

        /* Drain DTLS output into transmit queue */
        size_t dout_len = 0;
        while (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch),
                                &dout_len) == NANO_OK &&
               dout_len > 0) {
            nano_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANO_OUTPUT_TRANSMIT;
            tout.transmit.data = rtc->dtls_scratch;
            tout.transmit.len = dout_len;
            tout.transmit.dest = *src;
            rtc_enqueue_output(rtc, &tout);
            dout_len = 0;
        }

        /* Check for DTLS state transition → emit event */
        if (rtc->dtls.state == NANO_DTLS_STATE_ESTABLISHED &&
            rtc->state < NANO_STATE_DTLS_CONNECTED) {
            rtc->state = NANO_STATE_DTLS_CONNECTED;
            rtc->remote_addr = *src; /* save for timeout-driven output */

            nano_output_t devt;
            memset(&devt, 0, sizeof(devt));
            devt.type = NANO_OUTPUT_EVENT;
            devt.event.type = NANO_EVENT_DTLS_CONNECTED;
            rtc_enqueue_output(rtc, &devt);

            rtc_cache_fingerprint(rtc);

#if NANO_FEATURE_DATACHANNEL
            /* Initiate SCTP: DTLS client sends INIT (RFC 8831) */
            if (!rtc->dtls.is_server) {
                nsctp_start(&rtc->sctp);
                rtc->state = NANO_STATE_SCTP_CONNECTING;

                /* Drain SCTP output (INIT) through DTLS encrypt */
                rtc_pump_sctp_through_dtls(rtc, src);
            }
#else
            /* No DataChannel — DTLS connected is final state */
            rtc->state = NANO_STATE_CONNECTED;
#endif
        }

#if NANO_FEATURE_DATACHANNEL
        /* If DTLS is established, check for decrypted app data → SCTP */
        if (rtc->dtls.state == NANO_DTLS_STATE_ESTABLISHED) {
            const uint8_t *app_data = NULL;
            size_t app_len = 0;
            while (dtls_poll_app_data(&rtc->dtls, &app_data, &app_len) == NANO_OK && app_len > 0) {
                /* Feed decrypted data to SCTP */
                nsctp_handle_data(&rtc->sctp, app_data, app_len);

                /* Check for SCTP state transition */
                if (rtc->sctp.state == NANO_SCTP_STATE_ESTABLISHED &&
                    rtc->state < NANO_STATE_CONNECTED) {
                    rtc->state = NANO_STATE_CONNECTED;

                    nano_output_t sevt;
                    memset(&sevt, 0, sizeof(sevt));
                    sevt.type = NANO_OUTPUT_EVENT;
                    sevt.event.type = NANO_EVENT_SCTP_CONNECTED;
                    rtc_enqueue_output(rtc, &sevt);

                    NANO_LOGI("RTC", "SCTP established");
                }

                /* Deliver SCTP payload via DataChannel */
                if (rtc->sctp.has_delivered) {
                    dc_handle_message(&rtc->datachannel, rtc->sctp.delivered_stream,
                                      rtc->sctp.delivered_ppid, rtc->sctp.delivered_data,
                                      rtc->sctp.delivered_len);
                    rtc->sctp.has_delivered = false;

                    /* Emit DC events for data messages */
                    if (rtc->sctp.delivered_ppid == DCEP_PPID_STRING ||
                        rtc->sctp.delivered_ppid == DCEP_PPID_STRING_EMPTY) {
                        nano_output_t devt2;
                        memset(&devt2, 0, sizeof(devt2));
                        devt2.type = NANO_OUTPUT_EVENT;
                        devt2.event.type = NANO_EVENT_DATACHANNEL_STRING;
                        devt2.event.stream_id = rtc->sctp.delivered_stream;
                        devt2.event.data = rtc->sctp.delivered_data;
                        devt2.event.len = rtc->sctp.delivered_len;
                        rtc_enqueue_output(rtc, &devt2);
                    } else if (rtc->sctp.delivered_ppid == DCEP_PPID_BINARY ||
                               rtc->sctp.delivered_ppid == DCEP_PPID_BINARY_EMPTY) {
                        nano_output_t devt2;
                        memset(&devt2, 0, sizeof(devt2));
                        devt2.type = NANO_OUTPUT_EVENT;
                        devt2.event.type = NANO_EVENT_DATACHANNEL_DATA;
                        devt2.event.stream_id = rtc->sctp.delivered_stream;
                        devt2.event.data = rtc->sctp.delivered_data;
                        devt2.event.len = rtc->sctp.delivered_len;
                        rtc_enqueue_output(rtc, &devt2);
                    } else if (rtc->sctp.delivered_ppid == DCEP_PPID_CONTROL) {
                        /* DC OPEN triggers channel event */
                        nano_output_t devt2;
                        memset(&devt2, 0, sizeof(devt2));
                        devt2.type = NANO_OUTPUT_EVENT;
                        devt2.event.type = NANO_EVENT_DATACHANNEL_OPEN;
                        devt2.event.stream_id = rtc->sctp.delivered_stream;
                        rtc_enqueue_output(rtc, &devt2);
                    }
                }

                /* Drain SCTP output (SACK, handshake) through DTLS */
                rtc_pump_sctp_through_dtls(rtc, src);

                /* Also drain DC output (DCEP ACK) → SCTP → DTLS */
                uint8_t dc_buf[NANO_DC_OUT_BUF_SIZE];
                size_t dc_len = 0;
                uint16_t dc_stream = 0;
                while (dc_poll_output(&rtc->datachannel, dc_buf, sizeof(dc_buf), &dc_len,
                                      &dc_stream) == NANO_OK &&
                       dc_len > 0) {
                    nsctp_send(&rtc->sctp, dc_stream, DCEP_PPID_CONTROL, dc_buf, dc_len);
                    rtc_pump_sctp_through_dtls(rtc, src);
                    dc_len = 0;
                }

                app_len = 0;
            }
        }
#endif /* NANO_FEATURE_DATACHANNEL */

        return NANO_OK;

    } else if (first >= 128 && first <= 191) {
        /* RTP/RTCP [0x80-0xBF] */
        return NANO_ERR_NOT_IMPLEMENTED; /* Phase 2 */
    }

    return NANO_ERR_PROTOCOL; /* Unknown packet type */
}

/* ----------------------------------------------------------------
 * nano_handle_timeout — timer-driven state transitions
 * ---------------------------------------------------------------- */

int nano_handle_timeout(nano_rtc_t *rtc, uint32_t now_ms)
{
    if (!rtc) {
        return NANO_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;

    /* ICE: generate connectivity checks (controlling role) */
    if (rtc->ice.is_controlling && rtc->ice.state != NANO_ICE_STATE_CONNECTED &&
        rtc->ice.state != NANO_ICE_STATE_FAILED) {
        size_t out_len = 0;
        int rc = ice_generate_check(&rtc->ice, now_ms, rtc->config.crypto, rtc->stun_buf,
                                    sizeof(rtc->stun_buf), &out_len);
        if (rc != NANO_OK) {
            return rc;
        }

        if (out_len > 0) {
            nano_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANO_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = out_len;
            /* Destination: remote candidate address */
            out.transmit.dest.family = rtc->ice.remote_family;
            memcpy(out.transmit.dest.addr, rtc->ice.remote_addr, NANO_ADDR_SIZE);
            out.transmit.dest.port = rtc->ice.remote_port;
            rtc_enqueue_output(rtc, &out);
        }

        /* Schedule next timeout */
        if (rtc->ice.state == NANO_ICE_STATE_CHECKING) {
            nano_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANO_OUTPUT_TIMEOUT;
            tout.timeout_ms = rtc->ice.check_interval_ms;
            rtc_enqueue_output(rtc, &tout);
        }

        /* Propagate ICE failure */
        if (rtc->ice.state == NANO_ICE_STATE_FAILED) {
            rtc->state = NANO_STATE_CLOSED;
        }
    }

#if NANO_FEATURE_DATACHANNEL
    /* SCTP: retransmission + heartbeat timers */
    if (rtc->sctp.state == NANO_SCTP_STATE_ESTABLISHED) {
        nsctp_handle_timeout(&rtc->sctp, now_ms);

        /* Pump any SCTP output (retransmits, heartbeats, pending DATA) through DTLS */
        rtc_pump_sctp_through_dtls(rtc, &rtc->remote_addr);
    }
#endif

    return NANO_OK;
}

/* ----------------------------------------------------------------
 * DataChannel API
 * ---------------------------------------------------------------- */

#if NANO_FEATURE_DATACHANNEL
int nano_send_datachannel(nano_rtc_t *rtc, uint16_t stream_id, const void *data, size_t len)
{
    if (!rtc || !data) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANO_STATE_CONNECTED) {
        return NANO_ERR_STATE;
    }

    uint32_t ppid = (len > 0) ? DCEP_PPID_BINARY : DCEP_PPID_BINARY_EMPTY;
    return nsctp_send(&rtc->sctp, stream_id, ppid, (const uint8_t *)data, len);
}

int nano_send_datachannel_string(nano_rtc_t *rtc, uint16_t stream_id, const char *str)
{
    if (!rtc || !str) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANO_STATE_CONNECTED) {
        return NANO_ERR_STATE;
    }

    size_t len = strlen(str); /* NANO_SAFE: API boundary */

    uint32_t ppid = (len > 0) ? DCEP_PPID_STRING : DCEP_PPID_STRING_EMPTY;
    return nsctp_send(&rtc->sctp, stream_id, ppid, (const uint8_t *)str, len);
}
#endif /* NANO_FEATURE_DATACHANNEL */

#if NANO_FEATURE_AUDIO
int nano_send_audio(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len)
{
    (void)rtc;
    (void)timestamp;
    (void)data;
    (void)len;
    return NANO_ERR_NOT_IMPLEMENTED;
}
#endif

#if NANO_FEATURE_VIDEO
int nano_send_video(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len,
                    int is_keyframe)
{
    (void)rtc;
    (void)timestamp;
    (void)data;
    (void)len;
    (void)is_keyframe;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_request_keyframe(nano_rtc_t *rtc)
{
    (void)rtc;
    return NANO_ERR_NOT_IMPLEMENTED;
}
#endif

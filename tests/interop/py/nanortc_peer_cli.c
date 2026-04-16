/*
 * nanortc_peer_cli — line-protocol answerer driven via stdin/stdout.
 *
 * Used by the aiortc-based Python interop test suite (tests/interop/py/).
 * See protocol.md for the wire format. Always plays the answerer role.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Global state (single instance per process)
 * ---------------------------------------------------------------- */

static nanortc_t g_rtc;
static nano_run_loop_t g_loop;
static int g_initialized = 0;
static volatile int g_running = 1;

/* Scratch buffers (static to avoid large stack frames) */
#define STDIN_BUF_SIZE   16384
#define SCRATCH_SIZE     16384
#define B64_SCRATCH_SIZE 24576 /* 4/3 expansion + headroom */

static char g_stdin_buf[STDIN_BUF_SIZE];
static size_t g_stdin_len = 0;
static uint8_t g_scratch[SCRATCH_SIZE];
static char g_b64_out[B64_SCRATCH_SIZE];

/* ----------------------------------------------------------------
 * Base64 (standard alphabet, RFC 4648 §4)
 * ---------------------------------------------------------------- */

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    size_t out_len = ((len + 2) / 3) * 4;
    if (out_len + 1 > dst_size) {
        return -1;
    }

    size_t i = 0, j = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[j++] = b64_chars[(v >> 18) & 0x3F];
        dst[j++] = b64_chars[(v >> 12) & 0x3F];
        dst[j++] = b64_chars[(v >> 6) & 0x3F];
        dst[j++] = b64_chars[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)src[i + 1] << 8;
        }
        dst[j++] = b64_chars[(v >> 18) & 0x3F];
        dst[j++] = b64_chars[(v >> 12) & 0x3F];
        if (i + 1 < len) {
            dst[j++] = b64_chars[(v >> 6) & 0x3F];
        } else {
            dst[j++] = '=';
        }
        dst[j++] = '=';
    }
    dst[j] = '\0';
    return (int)j;
}

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static int b64_decode(const char *src, size_t len, uint8_t *dst, size_t dst_size)
{
    size_t j = 0;
    int bits = 0;
    uint32_t buf = 0;

    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            break;
        }
        int val = b64_decode_char(c);
        if (val < 0) {
            return -1;
        }
        buf = (buf << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (j >= dst_size) {
                return -1;
            }
            dst[j++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return (int)j;
}

/* ----------------------------------------------------------------
 * Output emission
 * ---------------------------------------------------------------- */

static void emit_line(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
}

static void emit_error(const char *msg)
{
    int n = b64_encode((const uint8_t *)msg, strlen(msg), g_b64_out, sizeof(g_b64_out));
    if (n >= 0) {
        fprintf(stdout, "ERROR %.*s\n", n, g_b64_out);
    } else {
        fputs("ERROR\n", stdout);
    }
    fprintf(stderr, "[cli] ERROR: %s\n", msg);
}

static void emit_error_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit_error(buf);
}

/* ----------------------------------------------------------------
 * nanortc event callback — translates events to wire events
 * ---------------------------------------------------------------- */

static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)rtc;
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE: {
        const char *name;
        switch (evt->ice_state) {
        case NANORTC_ICE_STATE_NEW:
            name = "new";
            break;
        case NANORTC_ICE_STATE_CHECKING:
            name = "checking";
            break;
        case NANORTC_ICE_STATE_CONNECTED:
            name = "connected";
            break;
        case NANORTC_ICE_STATE_DISCONNECTED:
            name = "disconnected";
            break;
        case NANORTC_ICE_STATE_FAILED:
            name = "failed";
            break;
        default:
            name = "unknown";
            break;
        }
        fprintf(stdout, "ICE_STATE %s\n", name);
        fprintf(stderr, "[cli] ICE_STATE %s\n", name);
        break;
    }

    case NANORTC_EV_CONNECTED:
        emit_line("CONNECTED");
        fprintf(stderr, "[cli] CONNECTED\n");
        break;

    case NANORTC_EV_DATACHANNEL_OPEN: {
        const char *label = evt->datachannel_open.label ? evt->datachannel_open.label : "";
        int n = b64_encode((const uint8_t *)label, strlen(label), g_b64_out, sizeof(g_b64_out));
        if (n < 0) {
            fprintf(stderr, "[cli] DC_OPEN: label too long to encode\n");
            break;
        }
        fprintf(stdout, "DC_OPEN %u %.*s\n", (unsigned)evt->datachannel_open.id, n, g_b64_out);
        fprintf(stderr, "[cli] DC_OPEN id=%u label=%s\n", (unsigned)evt->datachannel_open.id,
                label);
        break;
    }

    case NANORTC_EV_DATACHANNEL_DATA: {
        int n = b64_encode(evt->datachannel_data.data, evt->datachannel_data.len, g_b64_out,
                           sizeof(g_b64_out));
        if (n < 0) {
            fprintf(stderr, "[cli] DC_MESSAGE: payload too large (%zu bytes)\n",
                    evt->datachannel_data.len);
            break;
        }
        const char *kind = evt->datachannel_data.binary ? "BINARY" : "STRING";
        fprintf(stdout, "DC_MESSAGE %u %s %.*s\n", (unsigned)evt->datachannel_data.id, kind, n,
                g_b64_out);
        fprintf(stderr, "[cli] DC_MESSAGE id=%u %s (%zu bytes)\n",
                (unsigned)evt->datachannel_data.id, kind, evt->datachannel_data.len);
        break;
    }

    case NANORTC_EV_DATACHANNEL_CLOSE:
        fprintf(stdout, "DC_CLOSE %u\n", (unsigned)evt->datachannel_id.id);
        break;

    case NANORTC_EV_ICE_CANDIDATE: {
        const char *c = evt->ice_candidate.candidate_str;
        if (c && c[0]) {
            int n = b64_encode((const uint8_t *)c, strlen(c), g_b64_out, sizeof(g_b64_out));
            if (n >= 0) {
                fprintf(stdout, "LOCAL_CANDIDATE %.*s\n", n, g_b64_out);
            }
        }
        break;
    }

    case NANORTC_EV_DISCONNECTED:
        emit_line("ICE_STATE disconnected");
        fprintf(stderr, "[cli] DISCONNECTED\n");
        g_running = 0;
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------- */

static void log_cb(const nanortc_log_message_t *msg, void *ctx)
{
    (void)ctx;
    const char *lvl = "?";
    switch (msg->level) {
    case NANORTC_LOG_ERROR:
        lvl = "ERR";
        break;
    case NANORTC_LOG_WARN:
        lvl = "WRN";
        break;
    case NANORTC_LOG_INFO:
        lvl = "INF";
        break;
    case NANORTC_LOG_DEBUG:
        lvl = "DBG";
        break;
    case NANORTC_LOG_TRACE:
        lvl = "TRC";
        break;
    }
    fprintf(stderr, "[cli:%s] [%s] %s\n", lvl, msg->subsystem ? msg->subsystem : "?",
            msg->message ? msg->message : "");
}

static void cmd_init(const char *args)
{
    if (g_initialized) {
        emit_error("INIT: already initialized");
        return;
    }

    char *end;
    long port = strtol(args, &end, 10);
    if (port <= 0 || port > 65535) {
        emit_error("INIT: invalid port");
        return;
    }

    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nanortc_crypto_mbedtls();
    cfg.role = NANORTC_ROLE_CONTROLLED;

    /* Enable runtime log forwarding when NANORTC_PEER_CLI_LOG is set. */
    const char *log_env = getenv("NANORTC_PEER_CLI_LOG");
    if (log_env && log_env[0]) {
        cfg.log.callback = log_cb;
        cfg.log.user_data = NULL;
        if (strcmp(log_env, "trace") == 0) {
            cfg.log.level = NANORTC_LOG_TRACE;
        } else if (strcmp(log_env, "debug") == 0) {
            cfg.log.level = NANORTC_LOG_DEBUG;
        } else if (strcmp(log_env, "info") == 0) {
            cfg.log.level = NANORTC_LOG_INFO;
        } else {
            cfg.log.level = NANORTC_LOG_WARN;
        }
    }

    int rc = nanortc_init(&g_rtc, &cfg);
    if (rc != NANORTC_OK) {
        emit_error_fmt("nanortc_init failed: %d", rc);
        return;
    }

    rc = nano_run_loop_init(&g_loop, &g_rtc, (uint16_t)port);
    if (rc != 0) {
        nanortc_destroy(&g_rtc);
        emit_error_fmt("run_loop_init failed on port %ld", port);
        return;
    }
    /* Short select timeout so we drain stdin promptly between nanortc timer ticks. */
    g_loop.max_poll_ms = 20;
    nano_run_loop_set_event_cb(&g_loop, on_event, NULL);

    nanortc_add_local_candidate(&g_rtc, "127.0.0.1", (uint16_t)port);

    g_initialized = 1;
    emit_line("READY");
    fprintf(stderr, "[cli] INIT port=%ld ready\n", port);
}

static void cmd_set_offer(const char *args)
{
    if (!g_initialized) {
        emit_error("SET_OFFER: not initialized");
        return;
    }

    int sdp_len = b64_decode(args, strlen(args), g_scratch, sizeof(g_scratch) - 1);
    if (sdp_len < 0) {
        emit_error("SET_OFFER: invalid base64");
        return;
    }
    g_scratch[sdp_len] = '\0';

    if (getenv("NANORTC_PEER_CLI_DUMP_SDP")) {
        fprintf(stderr, "[cli] --- offer ---\n%s\n[cli] --- end offer ---\n",
                (const char *)g_scratch);
    }

    char answer[8192];
    size_t answer_len = 0;
    int rc =
        nanortc_accept_offer(&g_rtc, (const char *)g_scratch, answer, sizeof(answer), &answer_len);
    if (rc != NANORTC_OK) {
        emit_error_fmt("accept_offer failed: %d", rc);
        return;
    }

    if (getenv("NANORTC_PEER_CLI_DUMP_SDP")) {
        fprintf(stderr, "[cli] --- answer ---\n%s\n[cli] --- end answer ---\n", answer);
    }

    int n = b64_encode((const uint8_t *)answer, answer_len, g_b64_out, sizeof(g_b64_out));
    if (n < 0) {
        emit_error("SET_OFFER: answer too long to base64-encode");
        return;
    }
    fprintf(stdout, "LOCAL_ANSWER %.*s\n", n, g_b64_out);
    fprintf(stderr, "[cli] SET_OFFER -> LOCAL_ANSWER (%zu bytes)\n", answer_len);
}

static void cmd_add_candidate(const char *args)
{
    if (!g_initialized) {
        emit_error("ADD_CANDIDATE: not initialized");
        return;
    }

    int cand_len = b64_decode(args, strlen(args), g_scratch, sizeof(g_scratch) - 1);
    if (cand_len < 0) {
        emit_error("ADD_CANDIDATE: invalid base64");
        return;
    }
    g_scratch[cand_len] = '\0';

    int rc = nanortc_add_remote_candidate(&g_rtc, (const char *)g_scratch);
    if (rc != NANORTC_OK) {
        emit_error_fmt("add_remote_candidate failed: %d", rc);
    }
}

/* Parse "<id> <base64>" into (id, b64 pointer).
 * Returns 0 on success, -1 on parse error. Modifies args in place. */
static int parse_id_and_payload(char *args, uint16_t *out_id, const char **out_b64)
{
    char *space = strchr(args, ' ');
    if (!space) {
        return -1;
    }
    *space = '\0';
    long id = strtol(args, NULL, 10);
    if (id < 0 || id > 65535) {
        return -1;
    }
    *out_id = (uint16_t)id;
    *out_b64 = space + 1;
    return 0;
}

static void cmd_dc_send_string(char *args)
{
    if (!g_initialized) {
        emit_error("DC_SEND_STRING: not initialized");
        return;
    }

    uint16_t id = 0;
    const char *b64 = NULL;
    if (parse_id_and_payload(args, &id, &b64) != 0) {
        emit_error("DC_SEND_STRING: missing args");
        return;
    }

    int n = b64_decode(b64, strlen(b64), g_scratch, sizeof(g_scratch) - 1);
    if (n < 0) {
        emit_error("DC_SEND_STRING: invalid base64");
        return;
    }
    g_scratch[n] = '\0';

    int rc = nanortc_datachannel_send_string(&g_rtc, id, (const char *)g_scratch);
    if (rc != NANORTC_OK) {
        emit_error_fmt("datachannel_send_string failed: %d", rc);
    }
}

static void cmd_dc_send_binary(char *args)
{
    if (!g_initialized) {
        emit_error("DC_SEND_BINARY: not initialized");
        return;
    }

    uint16_t id = 0;
    const char *b64 = NULL;
    if (parse_id_and_payload(args, &id, &b64) != 0) {
        emit_error("DC_SEND_BINARY: missing args");
        return;
    }

    int n = b64_decode(b64, strlen(b64), g_scratch, sizeof(g_scratch));
    if (n < 0) {
        emit_error("DC_SEND_BINARY: invalid base64");
        return;
    }

    int rc = nanortc_datachannel_send(&g_rtc, id, g_scratch, (size_t)n);
    if (rc != NANORTC_OK) {
        emit_error_fmt("datachannel_send failed: %d", rc);
    }
}

static void cmd_shutdown(void)
{
    emit_line("SHUTDOWN_ACK");
    fprintf(stderr, "[cli] SHUTDOWN requested\n");
    g_running = 0;
}

/* ----------------------------------------------------------------
 * Line dispatcher
 * ---------------------------------------------------------------- */

static void dispatch_line(char *line)
{
    /* Strip trailing CR if any */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\r') {
        line[len - 1] = '\0';
    }
    if (line[0] == '\0') {
        return; /* blank line, ignore */
    }

    char *space = strchr(line, ' ');
    char *args = "";
    if (space) {
        *space = '\0';
        args = space + 1;
    }

    if (strcmp(line, "INIT") == 0) {
        cmd_init(args);
    } else if (strcmp(line, "SET_OFFER") == 0) {
        cmd_set_offer(args);
    } else if (strcmp(line, "ADD_CANDIDATE") == 0) {
        cmd_add_candidate(args);
    } else if (strcmp(line, "DC_SEND_STRING") == 0) {
        cmd_dc_send_string(args);
    } else if (strcmp(line, "DC_SEND_BINARY") == 0) {
        cmd_dc_send_binary(args);
    } else if (strcmp(line, "SHUTDOWN") == 0) {
        cmd_shutdown();
    } else {
        emit_error_fmt("unknown command: %s", line);
    }
}

/* ----------------------------------------------------------------
 * Main loop: interleave non-blocking stdin reads with run_loop_step
 * ---------------------------------------------------------------- */

static int poll_stdin_once(void)
{
    char buf[4096];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n > 0) {
        if (g_stdin_len + (size_t)n > sizeof(g_stdin_buf)) {
            emit_error("stdin line too long");
            return -1;
        }
        memcpy(g_stdin_buf + g_stdin_len, buf, (size_t)n);
        g_stdin_len += (size_t)n;

        /* Dispatch complete lines */
        size_t start = 0;
        for (size_t i = 0; i < g_stdin_len; i++) {
            if (g_stdin_buf[i] == '\n') {
                g_stdin_buf[i] = '\0';
                dispatch_line(g_stdin_buf + start);
                start = i + 1;
            }
        }
        if (start > 0) {
            size_t remaining = g_stdin_len - start;
            memmove(g_stdin_buf, g_stdin_buf + start, remaining);
            g_stdin_len = remaining;
        }
        return 0;
    }
    if (n == 0) {
        fprintf(stderr, "[cli] stdin EOF\n");
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    fprintf(stderr, "[cli] stdin read error: %d\n", errno);
    return -1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Line-buffered stdout so events flush promptly to the pipe. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* Non-blocking stdin */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "[cli] failed to set stdin non-blocking: %d\n", errno);
        return 1;
    }

    fprintf(stderr, "[cli] started (pid=%d)\n", (int)getpid());

    while (g_running) {
        if (poll_stdin_once() != 0) {
            break;
        }

        if (g_initialized) {
            nano_run_loop_step(&g_loop);
        } else {
            /* Idle before INIT arrives — short sleep to avoid busy spin. */
            struct timespec ts = {0, 2 * 1000 * 1000}; /* 2 ms */
            nanosleep(&ts, NULL);
        }
    }

    if (g_initialized) {
        nano_run_loop_destroy(&g_loop);
        nanortc_destroy(&g_rtc);
    }

    fprintf(stderr, "[cli] exiting cleanly\n");
    return 0;
}

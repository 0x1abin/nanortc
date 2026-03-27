/*
 * nanortc interop tests — Shared signaling and utility implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_common.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

/* ----------------------------------------------------------------
 * Signaling pipe
 * ---------------------------------------------------------------- */

int interop_sig_create(interop_sig_pipe_t *pipe)
{
    if (!pipe) {
        return -1;
    }
    return socketpair(AF_UNIX, SOCK_STREAM, 0, pipe->fd);
}

void interop_sig_destroy(interop_sig_pipe_t *pipe)
{
    if (!pipe) {
        return;
    }
    if (pipe->fd[0] >= 0) {
        close(pipe->fd[0]);
        pipe->fd[0] = -1;
    }
    if (pipe->fd[1] >= 0) {
        close(pipe->fd[1]);
        pipe->fd[1] = -1;
    }
}

int interop_sig_send(int fd, uint8_t type, const void *data, size_t len)
{
    if (fd < 0 || len > 0xFFFF) {
        return -1;
    }

    /* Header: type(1) + length(2 big-endian) */
    uint8_t hdr[3];
    hdr[0] = type;
    hdr[1] = (uint8_t)(len >> 8);
    hdr[2] = (uint8_t)(len & 0xFF);

    /* Send header */
    ssize_t n = write(fd, hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) {
        return -1;
    }

    /* Send payload */
    if (len > 0 && data) {
        const uint8_t *p = (const uint8_t *)data;
        size_t remaining = len;
        while (remaining > 0) {
            n = write(fd, p, remaining);
            if (n <= 0) {
                return -1;
            }
            p += n;
            remaining -= (size_t)n;
        }
    }

    return 0;
}

int interop_sig_recv(int fd, uint8_t *out_type, void *buf, size_t buf_len,
                     int timeout_ms)
{
    if (fd < 0 || !out_type || !buf) {
        return -1;
    }

    /* Wait for data with timeout */
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        return -1; /* timeout or error */
    }

    /* Read header: type(1) + length(2) */
    uint8_t hdr[3];
    ssize_t n = read(fd, hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) {
        return -1;
    }

    *out_type = hdr[0];
    size_t payload_len = ((size_t)hdr[1] << 8) | hdr[2];

    if (payload_len > buf_len) {
        return -1; /* buffer too small */
    }

    /* Read payload */
    if (payload_len > 0) {
        uint8_t *p = (uint8_t *)buf;
        size_t remaining = payload_len;
        while (remaining > 0) {
            pfd.revents = 0;
            ret = poll(&pfd, 1, timeout_ms);
            if (ret <= 0) {
                return -1;
            }
            n = read(fd, p, remaining);
            if (n <= 0) {
                return -1;
            }
            p += n;
            remaining -= (size_t)n;
        }
    }

    return (int)payload_len;
}

/* ----------------------------------------------------------------
 * Timing helpers
 * ---------------------------------------------------------------- */

uint32_t interop_get_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void interop_sleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

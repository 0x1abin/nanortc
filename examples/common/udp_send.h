/* Bounded, nonblocking UDP output ownership shared by the example loops. */
#ifndef NANO_UDP_SEND_H_
#define NANO_UDP_SEND_H_

#include "nanortc.h"
#include <errno.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#endif

#define NANO_UDP_TRANSPORT_CAPACITY \
    ((NANORTC_TX_SLOT_SIZE > NANORTC_TURN_BUF_SIZE) ? NANORTC_TX_SLOT_SIZE : NANORTC_TURN_BUF_SIZE)
#define NANO_UDP_PACKET_CAPACITY                                                        \
    ((NANO_UDP_TRANSPORT_CAPACITY > NANORTC_FEC_BUF_SIZE) ? NANO_UDP_TRANSPORT_CAPACITY \
                                                          : NANORTC_FEC_BUF_SIZE)
#define NANO_UDP_RETRY_LIMIT 3u

typedef struct {
    uint8_t data[NANO_UDP_PACKET_CAPACITY];
    struct sockaddr_storage dest;
    socklen_t dest_len;
    size_t len;
    uint64_t token;
    uint32_t started_ms;
    uint32_t retry_at_ms;
    uint32_t retries;
    uint32_t dropped;
    int fd;
    uint8_t attempts;
    bool pending;
} nano_udp_sender_t;

static inline bool nano_udp_transient(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS || error == ENOMEM ||
           error == EINTR;
}

static inline ssize_t nano_udp_sendto(int fd, const void *data, size_t len,
                                      const struct sockaddr *dest, socklen_t dest_len)
{
#ifdef MSG_DONTWAIT
    return sendto(fd, data, len, MSG_DONTWAIT, dest, dest_len);
#else
    /* The owning loop makes its socket nonblocking before the first send. */
    return sendto(fd, data, len, 0, dest, dest_len);
#endif
}

static inline void nano_udp_cancel(nano_udp_sender_t *sender, nanortc_t *rtc, uint32_t now_ms)
{
    if (sender->pending) {
        (void)nanortc_report_transmit(rtc, sender->token, NANORTC_TRANSMIT_DROPPED, now_ms);
        sender->dropped++;
        sender->pending = false;
    }
}

/* Called only with no pending packet. Copy only after a transient send failure. */
static inline void nano_udp_send(nano_udp_sender_t *sender, nanortc_t *rtc,
                                 const nanortc_output_t *out, int fd, const struct sockaddr *dest,
                                 socklen_t dest_len, uint32_t now_ms)
{
    ssize_t n = nano_udp_sendto(fd, out->transmit.data, out->transmit.len, dest, dest_len);
    if (n >= 0 && (size_t)n == out->transmit.len) {
        (void)nanortc_report_transmit(rtc, out->transmit.token, NANORTC_TRANSMIT_SENT, now_ms);
        return;
    }
    if (n >= 0 || !nano_udp_transient(errno) || out->transmit.len > sizeof(sender->data) ||
        dest_len > sizeof(sender->dest)) {
        (void)nanortc_report_transmit(rtc, out->transmit.token, NANORTC_TRANSMIT_DROPPED, now_ms);
        sender->dropped++;
        return;
    }
    memcpy(sender->data, out->transmit.data, out->transmit.len);
    memcpy(&sender->dest, dest, dest_len);
    sender->dest_len = dest_len;
    sender->len = out->transmit.len;
    sender->token = out->transmit.token;
    sender->fd = fd;
    sender->started_ms = now_ms;
    sender->retry_at_ms = now_ms + 1u;
    sender->attempts = 0;
    sender->pending = true;
}

static inline void nano_udp_retry(nano_udp_sender_t *sender, nanortc_t *rtc, uint32_t now_ms)
{
    if (!sender->pending || (int32_t)(now_ms - sender->retry_at_ms) < 0)
        return;
    if (now_ms - sender->started_ms > NANO_UDP_RETRY_LIMIT) {
        nano_udp_cancel(sender, rtc, now_ms);
        return;
    }
    sender->attempts++;
    sender->retries++;
    ssize_t n = nano_udp_sendto(sender->fd, sender->data, sender->len,
                                (const struct sockaddr *)&sender->dest, sender->dest_len);
    if (n >= 0 && (size_t)n == sender->len) {
        (void)nanortc_report_transmit(rtc, sender->token, NANORTC_TRANSMIT_SENT, now_ms);
        sender->pending = false;
    } else if (n < 0 && nano_udp_transient(errno) && sender->attempts < NANO_UDP_RETRY_LIMIT &&
               now_ms - sender->started_ms < NANO_UDP_RETRY_LIMIT) {
        sender->retry_at_ms = now_ms + 1u;
    } else {
        nano_udp_cancel(sender, rtc, now_ms);
    }
}

static inline uint32_t nano_udp_next_timeout(const nano_udp_sender_t *sender, uint32_t now_ms)
{
    if (!sender->pending)
        return UINT32_MAX;
    int32_t remaining = (int32_t)(sender->retry_at_ms - now_ms);
    return remaining > 0 ? (uint32_t)remaining : 0u;
}

#endif

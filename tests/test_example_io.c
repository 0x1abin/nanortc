/* Native, deterministic fault injection for the example I/O boundary.
 * Compile the actual helpers with socket/allocator shims: no real sockets,
 * DNS server, timing race or platform-specific LD_PRELOAD is required. */
#include "nanortc.h"
#include "nano_test.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

static int recv_error, select_result, select_error, recv_flags, ticks, inputs;
static int allocations, fail_alloc, fail_connect, sockets, closes, fcntl_flags, fail_fcntl;
static nanortc_input_t last_input;
static int output_pending;
static nanortc_output_t output;
static int sent_family;
static char wire[17000];
static size_t wire_len;
static const char *http_response;
static size_t response_offset;
static int mock_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    sockets++;
    return 17;
}
static int mock_close(int fd)
{
    (void)fd;
    closes++;
    return 0;
}
static int mock_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    (void)fd;
    (void)addr;
    (void)len;
    return fail_connect ? -1 : 0;
}
static int mock_setsockopt(int fd, int level, int name, const void *value, socklen_t size)
{
    (void)fd;
    (void)level;
    (void)name;
    (void)value;
    (void)size;
    return 0;
}
static int mock_fcntl(int fd, int command, ...)
{
    (void)fd;
    if (fail_fcntl)
        return -1;
    if (command == F_SETFL) {
        va_list ap;
        va_start(ap, command);
        fcntl_flags = va_arg(ap, int);
        va_end(ap);
    }
    return 0;
}
static void *mock_malloc(size_t size)
{
    if (fail_alloc)
        return NULL;
    void *p = malloc(size);
    if (p)
        allocations++;
    return p;
}
static void mock_free(void *p)
{
    if (p)
        allocations--;
    free(p);
}
static ssize_t mock_send(int fd, const void *buf, size_t size, int flags)
{
    (void)fd;
    (void)flags;
    if (wire_len + size >= sizeof(wire))
        return -1;
    memcpy(wire + wire_len, buf, size);
    wire_len += size;
    wire[wire_len] = 0;
    return (ssize_t)size;
}
static ssize_t mock_recv(int fd, void *buf, size_t size, int flags)
{
    (void)fd;
    (void)flags;
    size_t n = strlen(http_response) - response_offset;
    if (n > size)
        n = size;
    memcpy(buf, http_response + response_offset, n);
    response_offset += n;
    return (ssize_t)n;
}
static ssize_t mock_sendto(int fd, const void *buf, size_t len, int flags,
                           const struct sockaddr *dest, socklen_t size)
{
    (void)fd;
    (void)buf;
    (void)flags;
    (void)size;
    sent_family = dest->sa_family;
    return (ssize_t)len;
}
static ssize_t mock_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src,
                             socklen_t *size)
{
    (void)fd;
    (void)len;
    recv_flags = flags;
    if (recv_error) {
        errno = recv_error;
        return -1;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(9000)};
    inet_pton(AF_INET, "10.0.0.9", &addr.sin_addr);
    memcpy(src, &addr, sizeof(addr));
    *size = sizeof(addr);
    ((uint8_t *)buf)[0] = 0x80;
    return 1;
}
static int mock_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{
    (void)n;
    (void)r;
    (void)w;
    (void)e;
    (void)t;
    errno = select_error;
    return select_result;
}
static int mock_clock(clockid_t clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = 2;
    ts->tv_nsec = 5000000;
    return 0;
}
static int mock_input(nanortc_t *rtc, const nanortc_input_t *in)
{
    rtc->now_ms = in->now_ms;
    if (in->data) {
        last_input = *in;
        inputs++;
    } else
        ticks++;
    return NANORTC_OK;
}
static int mock_output(nanortc_t *rtc, nanortc_output_t *out)
{
    (void)rtc;
    if (!output_pending)
        return NANORTC_ERR_NO_DATA;
    *out = output;
    output_pending = 0;
    return NANORTC_OK;
}

#undef NANO_HAVE_GETIFADDRS
#define NANO_HAVE_GETIFADDRS 0
#ifdef TEST_EXAMPLE_FCNTL
#undef MSG_DONTWAIT
#endif
#define socket               mock_socket
#define close                mock_close
#define connect              mock_connect
#define setsockopt           mock_setsockopt
#define fcntl                mock_fcntl
#define malloc               mock_malloc
#define free                 mock_free
#define send                 mock_send
#define recv                 mock_recv
#define sendto               mock_sendto
#define recvfrom             mock_recvfrom
#define select               mock_select
#define clock_gettime        mock_clock
#define nanortc_handle_input mock_input
#define nanortc_poll_output  mock_output
#include "../examples/common/run_loop_linux.c"
#include "../examples/common/http_signaling.c"
#undef malloc
#undef free

TEST(test_example_recv_readiness_and_timer_progress)
{
    nanortc_t rtc = {0};
    nano_run_loop_t loop = {.rtc = &rtc, .fds = {17}, .fd_count = 1, .max_poll_ms = 1};
    select_result = 1;
    int errors[] = {EAGAIN, EWOULDBLOCK, EINTR, EIO};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        recv_error = errors[i];
        ticks = inputs = 0;
        ASSERT_EQ(nano_run_loop_step(&loop), errors[i] == EIO ? -1 : 0);
        ASSERT_EQ(ticks, 1);
        ASSERT_EQ(inputs, 0);
        ASSERT_EQ(rtc.now_ms, 2005);
        ASSERT_EQ(recv_flags, NANO_RECV_FLAGS);
    }
    select_result = -1;
    select_error = EINTR;
    ticks = 0;
    ASSERT_OK(nano_run_loop_step(&loop));
    ASSERT_EQ(ticks, 1);
    select_error = EBADF;
    ASSERT_EQ(nano_run_loop_step(&loop), -1);
    select_result = 1;
    recv_error = 0;
    ticks = inputs = 0;
    ASSERT_OK(nano_run_loop_step(&loop));
    ASSERT_EQ(last_input.src.family, 4);
    ASSERT_EQ(last_input.src.port, 9000);
    ASSERT_EQ(last_input.src.addr[3], 9);
    ASSERT_EQ(inputs, 1);
    ASSERT_EQ(ticks, 1);
}

TEST(test_example_socket_family_and_capability_fallback)
{
    nanortc_t rtc = {0};
    nano_run_loop_t loop = {.rtc = &rtc, .fds = {17}, .fd_count = 1};
    loop.local_addrs[0].family = 4;
    static const uint8_t packet[] = {0};
    output = (nanortc_output_t){.type = NANORTC_OUTPUT_TRANSMIT};
    output.transmit.data = packet;
    output.transmit.len = sizeof(packet);
    output.transmit.dest.family = 4;
    output.transmit.dest.port = 9000;
    output_pending = 1;
    nano_run_loop_drain(&loop);
    ASSERT_EQ(sent_family, AF_INET); /* Dedicated v4 fd, including IPv6 builds. */
#if NANORTC_FEATURE_IPV6
    loop.local_addrs[0].family = 0;
    output_pending = 1;
    nano_run_loop_drain(&loop);
    ASSERT_EQ(sent_family, AF_INET6); /* Wildcard dual-stack fd. */
#endif
    ASSERT_EQ(nano_run_loop_auto_candidates(&loop, &rtc, 9999), -1);
    ASSERT_EQ(loop.fd_count, 0);
    ASSERT_EQ(prepare_udp_socket(17), 17);
#ifdef TEST_EXAMPLE_FCNTL
    ASSERT_TRUE(fcntl_flags & O_NONBLOCK);
    fail_fcntl = 1;
    closes = 0;
    ASSERT_EQ(prepare_udp_socket(17), -1);
    ASSERT_EQ(closes, 1);
    fail_fcntl = 0;
#endif
}

TEST(test_example_http_buffer_failure_and_cleanup)
{
    http_sig_t sig = {.host = "127.0.0.1", .port = 8765, .peer_id = 1};
    char type[32], payload[128];
    allocations = sockets = closes = 0;
    fail_alloc = 1;
    ASSERT_EQ(http_sig_send(&sig, "candidate", "candidate:x", "candidate"), -1);
    ASSERT_EQ(http_sig_send_to(&sig, 2, "offer", "v=0", "sdp"), -1);
    ASSERT_EQ(http_sig_recv(&sig, type, sizeof(type), payload, sizeof(payload), 0), -1);
    ASSERT_EQ(allocations, 0);
    ASSERT_EQ(sockets, 0);
    fail_alloc = 0;
    fail_connect = 1;
    ASSERT_EQ(http_sig_send(&sig, "offer", "v=0", "sdp"), -1);
    ASSERT_EQ(http_sig_send_to(&sig, 2, "offer", "v=0", "sdp"), -1);
    ASSERT_EQ(http_sig_recv(&sig, type, sizeof(type), payload, sizeof(payload), 0), -1);
    ASSERT_EQ(allocations, 0);
    ASSERT_EQ(sockets, closes);
    fail_connect = 0;
    http_response = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
    response_offset = wire_len = 0;
    ASSERT_OK(http_sig_send_to(&sig, 2, "offer", "v=0\r\n", "sdp"));
    ASSERT_TRUE(strstr(wire, "POST /send?id=1&to=2 HTTP/1.0") != NULL);
    ASSERT_TRUE(strstr(wire, "v=0\\r\\n") != NULL);
    ASSERT_EQ(allocations, 0);
    ASSERT_EQ(sockets, closes);
    http_response = "HTTP/1.0 204 No Content\r\n\r\n";
    response_offset = 0;
    ASSERT_EQ(http_sig_recv(&sig, type, sizeof(type), payload, sizeof(payload), 0), -2);
    ASSERT_EQ(allocations, 0);
    ASSERT_EQ(sockets, closes);
    http_response = "HTTP/1.0 200 OK\r\n\r\n{\"type\":\"candidate\",\"candidate\":\"peer\"}";
    response_offset = 0;
    ASSERT_OK(http_sig_recv(&sig, type, sizeof(type), payload, sizeof(payload), 0));
    ASSERT_TRUE(strcmp(payload, "peer") == 0);
    ASSERT_EQ(allocations, 0);
    ASSERT_EQ(sockets, closes);
    static char oversized[HTTP_SIG_BUF_SIZE];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = 0;
    ASSERT_EQ(http_sig_send(&sig, "offer", oversized, "sdp"), -1);
    ASSERT_EQ(allocations, 0);
    ASSERT_EQ(sockets, closes);
}

TEST_MAIN_BEGIN("Example I/O")
RUN(test_example_recv_readiness_and_timer_progress);
RUN(test_example_socket_family_and_capability_fallback);
RUN(test_example_http_buffer_failure_and_cleanup);
TEST_MAIN_END

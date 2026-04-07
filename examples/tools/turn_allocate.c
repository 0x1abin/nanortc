/*
 * turn_allocate — Standalone TURN Allocate test tool
 *
 * Tests the TURN protocol against a real coturn server:
 *   1. Send Allocate Request (unauthenticated)
 *   2. Receive 401 + REALM + NONCE
 *   3. Send authenticated Allocate
 *   4. Receive Allocate Success + relay address
 *
 * Usage: turn_allocate <server> <port> <username> <password>
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_turn.h"
#include "nano_stun.h"
#include "nano_addr.h"
#include "nanortc_crypto.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Resolve hostname to IPv4 address */
static int resolve_host(const char *host, struct sockaddr_in *addr)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "DNS resolve failed: %s\n", gai_strerror(rc));
        return -1;
    }
    memcpy(addr, res->ai_addr, sizeof(*addr));
    freeaddrinfo(res);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <server> <port> <username> <password>\n", argv[0]);
        return 1;
    }

    const char *server = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *username = argv[3];
    const char *password = argv[4];

    printf("TURN Allocate test\n");
    printf("  Server: %s:%u\n", server, port);
    printf("  Username: %s\n", username);

    /* Get crypto provider */
#if defined(NANORTC_CRYPTO_OPENSSL)
    const nanortc_crypto_provider_t *crypto = nanortc_crypto_openssl();
#else
    const nanortc_crypto_provider_t *crypto = nanortc_crypto_mbedtls();
#endif

    /* Resolve server address */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    if (resolve_host(server, &server_addr) != 0) {
        return 1;
    }
    server_addr.sin_port = htons(port);
    printf("  Resolved: %s\n", inet_ntoa(server_addr.sin_addr));

    /* Extract binary address for TURN module */
    uint8_t server_bin[NANORTC_ADDR_SIZE] = {0};
    memcpy(server_bin, &server_addr.sin_addr.s_addr, 4);

    /* Create UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    /* Bind to any port */
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    /* Set receive timeout */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Initialize TURN module */
    nano_turn_t turn;
    turn_init(&turn);
    turn_configure(&turn, server_bin, 4, port,
                   username, strlen(username), password, strlen(password));

    /* Step 1: Send initial Allocate (unauthenticated) */
    printf("\n[1] Sending Allocate Request (unauthenticated)...\n");

    uint8_t buf[1024];
    size_t out_len = 0;
    int rc = turn_start_allocate(&turn, crypto, buf, sizeof(buf), &out_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "turn_start_allocate failed: %d\n", rc);
        close(sock);
        return 1;
    }

    ssize_t sent = sendto(sock, buf, out_len, 0,
                          (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sent < 0) {
        perror("sendto");
        close(sock);
        return 1;
    }
    printf("  Sent %zd bytes\n", sent);

    /* Step 2: Receive 401 response */
    printf("\n[2] Waiting for 401 challenge...\n");

    uint8_t recv_buf[2048];
    ssize_t received = recv(sock, recv_buf, sizeof(recv_buf), 0);
    if (received <= 0) {
        fprintf(stderr, "recv timeout or error\n");
        close(sock);
        return 1;
    }
    printf("  Received %zd bytes\n", received);

    rc = turn_handle_response(&turn, recv_buf, (size_t)received, crypto);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "turn_handle_response failed: %d\n", rc);
        close(sock);
        return 1;
    }

    if (turn.state != NANORTC_TURN_CHALLENGED) {
        fprintf(stderr, "Expected CHALLENGED state, got %d\n", turn.state);
        close(sock);
        return 1;
    }
    printf("  REALM: %s\n", turn.realm);
    printf("  NONCE: %.*s\n", (int)turn.nonce_len, turn.nonce);
    printf("  HMAC key derived: yes\n");

    /* Step 3: Send authenticated Allocate */
    printf("\n[3] Sending authenticated Allocate...\n");

    out_len = 0;
    rc = turn_start_allocate(&turn, crypto, buf, sizeof(buf), &out_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "turn_start_allocate (auth) failed: %d\n", rc);
        close(sock);
        return 1;
    }

    sent = sendto(sock, buf, out_len, 0,
                  (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sent < 0) {
        perror("sendto");
        close(sock);
        return 1;
    }
    printf("  Sent %zd bytes (with MI)\n", sent);

    /* Step 4: Receive Allocate Success */
    printf("\n[4] Waiting for Allocate Success...\n");

    received = recv(sock, recv_buf, sizeof(recv_buf), 0);
    if (received <= 0) {
        fprintf(stderr, "recv timeout or error\n");
        close(sock);
        return 1;
    }
    printf("  Received %zd bytes\n", received);

    rc = turn_handle_response(&turn, recv_buf, (size_t)received, crypto);
    if (rc != NANORTC_OK) {
        /* Parse raw to see error code */
        stun_msg_t msg;
        if (stun_parse(recv_buf, (size_t)received, &msg) == NANORTC_OK) {
            fprintf(stderr, "STUN type=0x%04x error=%u\n", msg.type, msg.error_code);
            if (msg.realm) {
                fprintf(stderr, "  realm=%.*s\n", (int)msg.realm_len, msg.realm);
            }
        }
        fprintf(stderr, "turn_handle_response failed: %d (%s)\n", rc, nanortc_err_name(rc));
        close(sock);
        return 1;
    }

    if (turn.state == NANORTC_TURN_ALLOCATED) {
        /* Format relay address */
        char relay_str[NANORTC_IPV6_STR_SIZE];
        if (turn.relay_family == STUN_FAMILY_IPV4) {
            snprintf(relay_str, sizeof(relay_str), "%u.%u.%u.%u",
                     turn.relay_addr[0], turn.relay_addr[1],
                     turn.relay_addr[2], turn.relay_addr[3]);
        } else {
            snprintf(relay_str, sizeof(relay_str), "(IPv6)");
        }
        printf("\n=== TURN ALLOCATE SUCCESS ===\n");
        printf("  Relay address: %s:%u\n", relay_str, turn.relay_port);
        printf("  Lifetime: %u seconds\n", turn.lifetime_s);
        printf("  State: ALLOCATED\n");
    } else {
        fprintf(stderr, "Unexpected state: %d\n", turn.state);
        close(sock);
        return 1;
    }

    close(sock);
    printf("\nDone.\n");
    return 0;
}

/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * linktool - a tiny static TCP echo client/server to prove real data passes
 * between two QEMU i.MX 93 instances over a socket-bridged link. Avoids
 * getaddrinfo/NSS so it links -static cleanly with the aarch64 cross gcc.
 *
 *   linktool server <port>                 # accept one conn, echo bytes back
 *   linktool client <ip> <port> <payload>  # send payload, verify echo matches
 *
 * Emits a single LINK:PASS:<tag> / LINK:FAIL:<tag>:<why> marker on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int read_n(int fd, char *buf, int n)
{
    int got = 0, r;

    while (got < n && (r = read(fd, buf + got, n - got)) > 0) {
        got += r;
    }
    return got;
}

static int run_server(int port)
{
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    int ls = socket(AF_INET, SOCK_STREAM, 0), opt = 1, cs, n;
    char buf[1024];

    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1)) {
        printf("LINK:FAIL:server:bind\n");
        return 1;
    }
    printf("LINK:SERVER-READY\n");
    fflush(stdout);

    cs = accept(ls, NULL, NULL);
    if (cs < 0) {
        printf("LINK:FAIL:server:accept\n");
        return 1;
    }
    n = read(cs, buf, sizeof(buf) - 1);
    if (n <= 0) {
        printf("LINK:FAIL:server:read\n");
        return 1;
    }
    if (write(cs, buf, n) != n) {           /* echo it back */
        printf("LINK:FAIL:server:write\n");
        return 1;
    }
    buf[n] = 0;
    printf("LINK:PASS:server:echoed %d bytes [%s]\n", n, buf);
    return 0;
}

static int run_client(const char *ip, int port, const char *payload)
{
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    int plen = strlen(payload), cs = -1, tries, n;
    char buf[1024];

    inet_pton(AF_INET, ip, &a.sin_addr);
    for (tries = 0; tries < 60; tries++) {  /* retry: ARP/boot warmup */
        cs = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(cs, (struct sockaddr *)&a, sizeof(a)) == 0) {
            break;
        }
        close(cs);
        cs = -1;
        sleep(1);
    }
    if (cs < 0) {
        printf("LINK:FAIL:client:connect\n");
        return 1;
    }
    if (write(cs, payload, plen) != plen) {
        printf("LINK:FAIL:client:write\n");
        return 1;
    }
    n = read_n(cs, buf, plen);
    buf[n < 1024 ? n : 1023] = 0;
    if (n == plen && !memcmp(buf, payload, plen)) {
        printf("LINK:PASS:client:echo byte-exact (%d bytes)\n", plen);
        return 0;
    }
    printf("LINK:FAIL:client:mismatch got %d [%s]\n", n, buf);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "server")) {
        return run_server(atoi(argv[2]));
    }
    if (argc >= 5 && !strcmp(argv[1], "client")) {
        return run_client(argv[2], atoi(argv[3]), argv[4]);
    }
    printf("usage: linktool server <port> | client <ip> <port> <payload>\n");
    return 2;
}

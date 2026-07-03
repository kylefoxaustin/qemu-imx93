/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * i2clink - i2c-dev oracle for the i.MX 93 I2C board-to-board link
 * (run-i2c.sh). Each board's LPI2C is the master talking to a local i2c-link
 * target (at a fixed address) that bridges the bus to a socket. The SENDER
 * master WRITES a payload to the link address (the link forwards it to the
 * peer); the RECEIVER master READS from the link address (the link returns the
 * bytes the peer wrote) and verifies byte-exact. Idle reads return 0xff (the
 * payload has none), so the receiver searches its read stream for the payload.
 * Raw I2C_RDWR ioctls, no libs, so it links -static for a busybox initramfs.
 * Emits I2CLINK:PASS / I2CLINK:FAIL / I2CLINK:SENT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static int i2c_xfer(int fd, int addr, int rd, unsigned char *buf, int len)
{
    struct i2c_msg msg = {
        .addr = addr,
        .flags = rd ? I2C_M_RD : 0,
        .len = len,
        .buf = buf,
    };
    struct i2c_rdwr_ioctl_data d = { .msgs = &msg, .nmsgs = 1 };

    return ioctl(fd, I2C_RDWR, &d);
}

int main(int argc, char **argv)
{
    const char *role = argc > 1 ? argv[1] : "";
    const char *dev = argc > 2 ? argv[2] : "/dev/i2c-3";
    int addr = argc > 3 ? (int)strtol(argv[3], NULL, 0) : 0x42;
    const char *payload = argc > 4 ? argv[4] :
                          "IMX93-I2C-LINK-payload-0123456789";
    int plen = strlen(payload), fd, i, got = 0;
    unsigned char rx[64], collected[512];
    time_t start;

    fd = open(dev, O_RDWR);
    if (fd < 0) { printf("I2CLINK:FAIL:open %s (%s)\n", dev, strerror(errno)); return 1; }

    if (!strcmp(role, "send")) {
        if (i2c_xfer(fd, addr, 0, (unsigned char *)payload, plen) < 0) {
            printf("I2CLINK:FAIL:send xfer errno=%d\n", errno); return 1;
        }
        printf("I2CLINK:SENT %d bytes [%s] -> 0x%02x\n", plen, payload, addr);
        return 0;
    }

    /* receive: read chunks, collect, search for the payload as a substring */
    start = time(NULL);
    while (got < (int)sizeof(collected) && time(NULL) - start < 45) {
        int n = i2c_xfer(fd, addr, 1, rx, sizeof(rx));

        if (n < 0) { printf("I2CLINK:FAIL:recv xfer errno=%d\n", errno); return 1; }
        for (i = 0; i < (int)sizeof(rx) && got < (int)sizeof(collected); i++) {
            if (rx[i] != 0xff) {
                collected[got++] = rx[i];
            }
        }
        for (i = 0; i + plen <= got; i++) {
            if (!memcmp(collected + i, payload, plen)) {
                printf("I2CLINK:PASS: %d bytes crossed the I2C link byte-exact\n",
                       plen);
                return 0;
            }
        }
        usleep(100000);
    }
    printf("I2CLINK:FAIL: got %d bytes, payload not found\n", got);
    return 1;
}

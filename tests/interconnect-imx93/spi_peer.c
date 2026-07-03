/*
 * spi_peer - i.MX 93 Linux spidev peer for the MCXN947 bare-metal SPI node
 * (tests/mcxn-spi-link). Cross-SoC validation of the shared spi_link.c: the
 * MCX M33 master clocks a 0x5A MOSI stream and seeks a 0xA5 marker + 32-byte
 * pattern from the peer. This 93 end (an fsl-lpspi master over /dev/spidev)
 * clocks that frame out (MOSI -> the MCX collects it) while draining the MCX's
 * 0x5A stream in (MISO -> we verify it). Matches mcx's spi_peer.py frame:
 * [0xA5] + [(i*3+5)&0x7F for i in 0..31].
 *
 * Clocks in <=16-byte chunks (the imx93 LPSPI model RX FIFO is 16 deep).
 * Emits SPIPEER:RXOK / SPIPEER:FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/spi/spidev.h>

#define N 32
#define FRAME_LEN (1 + N)
#define CHUNK 8

static int xfer(int fd, const unsigned char *tx, unsigned char *rx, int len)
{
    struct spi_ioc_transfer tr;

    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = len;
    tr.bits_per_word = 8;
    tr.speed_hz = 1000000;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/spidev0.0";
    unsigned char frame[FRAME_LEN], rx[CHUNK];
    unsigned char mode = 0, bits = 8;
    unsigned int speed = 1000000;
    long mosi_total = 0, mosi_bad = 0;
    time_t start;
    int fd, i, off;

    frame[0] = 0xA5;
    for (i = 0; i < N; i++) {
        frame[1 + i] = (unsigned char)((i * 3 + 5) & 0x7F);
    }

    fd = open(dev, O_RDWR);
    if (fd < 0) { printf("SPIPEER:FAIL:open %s\n", dev); return 1; }
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        printf("SPIPEER:FAIL:spidev setup\n"); return 1;
    }

    /*
     * The MCX firmware is one-shot: it clocks its 0x5A stream while seeking our
     * 0xA5 marker + 32 bytes, then stops. So we clock a BOUNDED byte count once
     * (the frame + padding, well under the MCX spi-link's 256-deep FIFO) - not
     * a busy loop, or once the MCX stops draining, its FIFO fills, backpressure
     * reaches our socket write and the LPSPI TDR write blocks. We build one
     * TX buffer (frame then 0x00 padding) and clock it in <=8B chunks (RX FIFO
     * is 16 deep), reading the MCX's 0x5A stream in as MISO.
     */
    {
        unsigned char tx[128];
        int total = (int)sizeof(tx);

        memset(tx, 0, sizeof(tx));
        memcpy(tx, frame, FRAME_LEN);
        (void)start;
        for (off = 0; off < total; off += CHUNK) {
            int len = total - off < CHUNK ? total - off : CHUNK;
            if (xfer(fd, tx + off, rx, len) < 0) {
                printf("SPIPEER:FAIL:xfer errno=%d\n", errno); return 1;
            }
            for (i = 0; i < len; i++) {
                if (rx[i] != 0xff) {   /* 0xff = idle (no peer byte queued) */
                    mosi_total++;
                    if (rx[i] != 0x5A) mosi_bad++;
                }
            }
            usleep(20000);            /* let the MCX clock its stream in */
        }
    }

    printf("SPIPEER: MOSI-in %ld bytes, %ld not-0x5A\n", mosi_total, mosi_bad);
    if (mosi_total >= N && mosi_bad == 0) {
        printf("SPIPEER:RXOK drained the MCX 0x5A stream byte-exact\n");
        return 0;
    }
    printf("SPIPEER:FAIL: %ld/%ld bad (or too few)\n", mosi_bad, mosi_total);
    return 1;
}

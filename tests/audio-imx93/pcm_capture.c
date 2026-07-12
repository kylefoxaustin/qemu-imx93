/*
 * Minimal ALSA capture oracle for the i.MX93 SAI3 (wm8962) card.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Records from an ALSA PCM device (default hw:1,0, the wm8962/SAI3 card),
 * driving the SAI3 RX FIFO -> eDMA3 cyclic (device->memory) datapath. The SAI
 * model synthesises a sawtooth into its RX FIFO, so a working capture path
 * delivers a non-silent, changing signal to userspace. A clean "PASS (N frames,
 * M distinct levels)" means the eDMA drained RDR0 into the ring at the audio
 * rate without over-running and real samples reached readi.
 *
 * There is no arecord in the BSP, hence this. Cross-compile against an ALSA
 * sysroot (see run.sh). NOTE: snd_pcm_set_params leaves the stream PREPARED but
 * does NOT auto-start the first readi - you MUST snd_pcm_start() explicitly, or
 * fsl-sai never sets RCSR.RE and the read returns EIO with no data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    unsigned int rate = 48000, chans = 2;
    unsigned int secs = argc > 2 ? atoi(argv[2]) : 1;
    snd_pcm_t *pcm;
    int err, i, bytes = 2;
    long frames = (long)rate * secs;
    unsigned char *buf;
    snd_pcm_sframes_t r;
    long nonzero = 0, distinct = 0;
    int seen[512];
    int nseen = 0;
    snd_pcm_format_t fmt = SND_PCM_FORMAT_S16_LE;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        printf("CAP[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    /* MICFIL only offers S32_LE; the wm8962/SAI card takes S16_LE. Try both. */
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate,
                             1, 500000);
    if (err < 0) {
        fmt = SND_PCM_FORMAT_S32_LE;
        bytes = 4;
        err = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 chans, rate, 1, 500000);
    }
    if (err < 0) {
        printf("CAP[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("CAP[%s]: format %s (%d bytes/sample)\n", dev,
           snd_pcm_format_name(fmt), bytes);
    /* Critical: PREPARED does not auto-start capture; start it explicitly. */
    err = snd_pcm_start(pcm);
    if (err < 0) {
        printf("CAP[%s]: start: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("CAP[%s]: %u Hz %u ch S16_LE, reading %ld frames\n", dev, rate,
           chans, frames);

    buf = malloc((size_t)frames * chans * bytes);
    r = snd_pcm_readi(pcm, buf, frames);
    printf("CAP[%s]: readi -> %ld\n", dev, (long)r);
    if (r < 0) {
        printf("CAP[%s]: FAIL (%s)\n", dev, snd_strerror((int)r));
        return 1;
    }

    /* Count non-zero samples and distinct levels (a real signal varies). */
    for (i = 0; i < r * (int)chans; i++) {
        const unsigned char *p = buf + (size_t)i * bytes;
        int v, j, found = 0;
        if (bytes == 2) {
            v = (short)(p[0] | (p[1] << 8));
        } else {
            unsigned u = p[0] | (p[1] << 8) | (p[2] << 16) |
                         ((unsigned)p[3] << 24);
            v = (int)u >> 16;   /* synthesised ramp is in the high 16 bits */
        }
        if (v != 0) {
            nonzero++;
        }
        for (j = 0; j < nseen; j++) {
            if (seen[j] == v) {
                found = 1;
                break;
            }
        }
        if (!found && nseen < 512) {
            seen[nseen++] = v;
            distinct++;
        }
    }
    printf("CAP[%s]: first level samples: %ld %ld %ld %ld\n", dev,
           (long)nseen, distinct > 0 ? (long)seen[0] : 0,
           distinct > 1 ? (long)seen[1] : 0, distinct > 2 ? (long)seen[2] : 0);
    snd_pcm_close(pcm);

    /* A working capture path delivers a non-silent, varying signal. */
    if (r > 0 && nonzero > 0 && distinct > 4) {
        printf("CAP[%s]: PASS (%ld frames, %ld distinct levels, %ld nonzero)\n",
               dev, (long)r, distinct, nonzero);
        return 0;
    }
    printf("CAP[%s]: FAIL (silent/flat: %ld frames, %ld distinct)\n",
           dev, (long)r, distinct);
    return 1;
}

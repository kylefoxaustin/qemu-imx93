/*
 * Minimal ALSA playback oracle for the i.MX93 SAI3/wm8962 card.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Plays a generated square wave to an ALSA PCM device (default hw:1,0, the
 * wm8962/SAI3 card), driving the eDMA3 cyclic -> SAI3 TX FIFO datapath. A clean
 * "PASS (N frames)" with drain success means the DMA paced the whole stream at
 * the audio rate without under-running. There is no aplay in the BSP, hence
 * this. Cross-compile against an ALSA sysroot (see run.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    unsigned int secs = argc > 2 ? atoi(argv[2]) : 2;
    /*
     * The rate is an ARGUMENT, not a constant. A SAI that assumes one rate is
     * invisible to a test that only ever asks for that rate - so the test has
     * to be able to ask for another one.
     */
    unsigned int rate = argc > 3 ? (unsigned int)atoi(argv[3]) : 48000;
    unsigned int chans = 2;
    snd_pcm_t *pcm;
    int err, i, bytes = 2;
    long frames = (long)rate * secs;
    void *buf;
    snd_pcm_sframes_t w;
    snd_pcm_format_t fmt = SND_PCM_FORMAT_S16_LE;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("PLAY[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    /* SPDIF (xcvr) and MICFIL want S32_LE; the wm8962/SAI card takes S16_LE. */
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate,
                             1, 200000);
    if (err < 0) {
        fmt = SND_PCM_FORMAT_S32_LE;
        bytes = 4;
        err = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 chans, rate, 1, 200000);
    }
    if (err < 0) {
        printf("PLAY[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("PLAY[%s]: %u Hz %u ch %s, %ld frames\n", dev, rate, chans,
           snd_pcm_format_name(fmt), frames);

    /* 440 Hz square wave: period ~110 frames at 48 kHz. */
    buf = malloc((size_t)frames * chans * bytes);
    for (i = 0; i < frames; i++) {
        int hi = ((i / 55) & 1) ? 8000 : -8000;
        if (bytes == 2) {
            ((short *)buf)[i * 2] = (short)hi;
            ((short *)buf)[i * 2 + 1] = (short)hi;
        } else {
            ((int *)buf)[i * 2] = hi << 16;       /* square wave in high bits */
            ((int *)buf)[i * 2 + 1] = hi << 16;
        }
    }

    w = snd_pcm_writei(pcm, buf, frames);
    printf("PLAY[%s]: writei -> %ld\n", dev, (long)w);
    if (w < 0) {
        printf("PLAY[%s]: FAIL (%s)\n", dev, snd_strerror((int)w));
        return 1;
    }
    err = snd_pcm_drain(pcm);
    printf("PLAY[%s]: drain -> %s\n", dev, snd_strerror(err));
    snd_pcm_close(pcm);
    printf("PLAY[%s]: %s (%ld frames)\n", dev,
           w == frames ? "PASS" : "PARTIAL", (long)w);
    return 0;
}

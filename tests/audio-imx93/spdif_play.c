/*
 * Minimal ALSA SPDIF player for the i.MX93 XCVR card.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Plays an IEC958 stream to the XCVR (SPDIF) card, driving the TFDR -> eDMA2
 * cyclic transmit datapath the same way the fsl_xcvr driver does. The XCVR PCM
 * only offers SND_PCM_FORMAT_IEC958_SUBFRAME_LE at 32 kHz .. 192 kHz stereo -
 * S16/S32 set_params is rejected - which is why the wm8962 pcm_play oracle
 * cannot drive it and this exists. soft_resample is disabled so the hardware
 * runs at exactly the requested rate, and the driver programs spdif_root to
 * rate * 128 (the SPDIF biphase clock). A clean "PASS" means the eDMA paced the
 * whole stream out of the TX FIFO without under-running.
 *
 * There is no aplay in the BSP; cross-compile against an ALSA sysroot (run.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:2,0";
    unsigned int rate = argc > 2 ? (unsigned int)atoi(argv[2]) : 48000;
    unsigned int chans = 2;
    snd_pcm_t *pcm;
    int err, i;
    long frames = rate;                 /* one second */
    int *buf;
    snd_pcm_sframes_t w;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("SPDIF[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    /* IEC958 subframe, no soft resample: the HW rate is exactly what we ask. */
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_IEC958_SUBFRAME_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate, 0,
                             200000);
    if (err < 0) {
        printf("SPDIF[%s]: set_params(%u Hz): %s\n", dev, rate,
               snd_strerror(err));
        return 1;
    }
    printf("SPDIF[%s]: %u Hz %u ch IEC958, %ld frames\n", dev, rate, chans,
           frames);

    buf = malloc((size_t)frames * chans * 4);
    for (i = 0; i < frames; i++) {
        int hi = ((i / 55) & 1) ? 8000 : -8000;   /* square wave, high bits */
        buf[i * 2] = hi << 16;
        buf[i * 2 + 1] = hi << 16;
    }

    w = snd_pcm_writei(pcm, buf, frames);
    printf("SPDIF[%s]: writei -> %ld\n", dev, (long)w);
    if (w < 0) {
        printf("SPDIF[%s]: FAIL (%s)\n", dev, snd_strerror((int)w));
        return 1;
    }
    err = snd_pcm_drain(pcm);
    printf("SPDIF[%s]: drain -> %s\n", dev, snd_strerror(err));
    snd_pcm_close(pcm);
    printf("SPDIF[%s]: %s (%ld frames at %u Hz)\n", dev,
           w == frames ? "PASS" : "PARTIAL", (long)w, rate);
    return 0;
}

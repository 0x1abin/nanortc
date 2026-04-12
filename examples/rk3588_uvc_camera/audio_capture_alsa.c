/*
 * audio_capture_alsa.c — ALSA capture + libopus encoder
 *
 * Opens a PCM capture device (typically the USB Audio Class microphone
 * that ships with the UVC camera), reads 20 ms periods of S16_LE PCM,
 * encodes each one to an Opus packet, and hands the packet to a
 * user-supplied callback.
 *
 * Runs on a dedicated pthread so the blocking snd_pcm_readi() does not
 * stall the main select() loop. All ALSA operations are performed
 * *inside* the audio thread (open, hw_params, read, close) because some
 * USB audio drivers get unhappy when hw params and reads happen from
 * different threads (we hit EIO on every read in that setup).
 *
 * Shutdown uses SIGUSR1 + pthread_kill to interrupt the blocking read;
 * the main thread is expected to block SIGUSR1 before calling
 * audio_start() so the signal is only delivered to this thread.
 *
 * SPDX-License-Identifier: MIT
 */

#include "audio_capture.h"

#include <alsa/asoundlib.h>
#include <opus/opus.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

static struct {
    pthread_t tid;
    volatile sig_atomic_t quit;
    bool running;
    audio_config_t cfg;
    int frame_samples; /**< samples per channel per frame (sample_rate * frame_ms / 1000) */
    /* Open/init result signalled back to audio_start via this atomic. */
    atomic_int init_state; /* 0=pending, 1=ok, 2=failed */
} g_audio;

/* Empty handler — exists only so SIGUSR1 interrupts snd_pcm_readi via EINTR. */
static void audio_sig_handler(int sig)
{
    (void)sig;
}

/* Open ALSA capture device with explicit hw_params + sw_params. Returns
 * the opened handle on success, NULL on failure (with reason logged). */
static snd_pcm_t *alsa_open(const char *device, unsigned int rate, unsigned int channels,
                            snd_pcm_uframes_t period_frames)
{
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[audio] snd_pcm_open(%s): %s\n", device, snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);

    err = snd_pcm_hw_params_any(pcm, hw);
    if (err < 0) {
        fprintf(stderr, "[audio] hw_params_any: %s\n", snd_strerror(err));
        goto fail;
    }

    err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "[audio] set_access: %s\n", snd_strerror(err));
        goto fail;
    }

    err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        fprintf(stderr, "[audio] set_format S16_LE: %s\n", snd_strerror(err));
        goto fail;
    }

    err = snd_pcm_hw_params_set_channels(pcm, hw, channels);
    if (err < 0) {
        fprintf(stderr, "[audio] set_channels %u: %s\n", channels, snd_strerror(err));
        goto fail;
    }

    unsigned int rrate = rate;
    err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rrate, 0);
    if (err < 0) {
        fprintf(stderr, "[audio] set_rate_near %u: %s\n", rate, snd_strerror(err));
        goto fail;
    }
    if (rrate != rate) {
        fprintf(stderr, "[audio] warning: rate %u → %u\n", rate, rrate);
    }

    snd_pcm_uframes_t period = period_frames;
    int dir = 0;
    err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
    if (err < 0) {
        fprintf(stderr, "[audio] set_period_size_near %lu: %s\n",
                (unsigned long)period_frames, snd_strerror(err));
        goto fail;
    }

    /* 4-period ring buffer (~80ms of headroom at 20ms periods). */
    snd_pcm_uframes_t buffer = period * 4;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if (err < 0) {
        fprintf(stderr, "[audio] set_buffer_size_near: %s\n", snd_strerror(err));
        goto fail;
    }

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) {
        fprintf(stderr, "[audio] hw_params apply: %s\n", snd_strerror(err));
        goto fail;
    }

    snd_pcm_uframes_t got_period = 0, got_buffer = 0;
    snd_pcm_hw_params_get_period_size(hw, &got_period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &got_buffer);

    /* Software params: start auto when buffer is half-full, report avail
     * at one period boundary. */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    err = snd_pcm_sw_params_current(pcm, sw);
    if (err < 0) {
        fprintf(stderr, "[audio] sw_params_current: %s\n", snd_strerror(err));
        goto fail;
    }
    snd_pcm_sw_params_set_start_threshold(pcm, sw, got_period);
    snd_pcm_sw_params_set_avail_min(pcm, sw, got_period);
    err = snd_pcm_sw_params(pcm, sw);
    if (err < 0) {
        fprintf(stderr, "[audio] sw_params apply: %s\n", snd_strerror(err));
        goto fail;
    }

    err = snd_pcm_prepare(pcm);
    if (err < 0) {
        fprintf(stderr, "[audio] prepare: %s\n", snd_strerror(err));
        goto fail;
    }

    err = snd_pcm_start(pcm);
    if (err < 0) {
        fprintf(stderr, "[audio] start: %s\n", snd_strerror(err));
        goto fail;
    }

    fprintf(stderr, "[audio] ALSA opened %s (%u Hz, %u ch, period=%lu, buffer=%lu)\n",
            device, rrate, channels, (unsigned long)got_period, (unsigned long)got_buffer);
    return pcm;

fail:
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

static void *audio_thread_fn(void *arg)
{
    (void)arg;

    /* Unblock SIGUSR1 on this thread only. Main thread is expected to
     * have blocked it process-wide via pthread_sigmask before spawning us. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    int ch = g_audio.cfg.channels;
    int nsamples = g_audio.frame_samples;
    unsigned int rate = (unsigned int)g_audio.cfg.sample_rate;

    /* 1. Open ALSA from *inside* this thread (not from audio_start). */
    snd_pcm_t *pcm = alsa_open(g_audio.cfg.device, rate, (unsigned int)ch,
                                (snd_pcm_uframes_t)nsamples);
    if (!pcm) {
        atomic_store(&g_audio.init_state, 2);
        return NULL;
    }

    /* 2. Create Opus encoder. */
    int oerr = 0;
    OpusEncoder *enc = opus_encoder_create((int)rate, ch, OPUS_APPLICATION_VOIP, &oerr);
    if (!enc || oerr != OPUS_OK) {
        fprintf(stderr, "[audio] opus_encoder_create: %s\n", opus_strerror(oerr));
        if (enc) opus_encoder_destroy(enc);
        snd_pcm_close(pcm);
        atomic_store(&g_audio.init_state, 2);
        return NULL;
    }
    int bitrate = g_audio.cfg.bitrate_bps > 0 ? g_audio.cfg.bitrate_bps : 64000;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
    fprintf(stderr, "[audio] opus encoder: %d bps %s voip\n", bitrate, ch == 2 ? "stereo" : "mono");

    /* Signal audio_start that we're ready. */
    atomic_store(&g_audio.init_state, 1);

    size_t pcm_bytes = (size_t)nsamples * (size_t)ch * sizeof(int16_t);
    int16_t *pcm_buf = (int16_t *)malloc(pcm_bytes);
    if (!pcm_buf) {
        fprintf(stderr, "[audio] pcm_buf alloc failed\n");
        goto cleanup;
    }
    uint8_t opus_buf[1500]; /* max Opus 20ms frame ≈ 1275 bytes */

    fprintf(stderr, "[audio] capture thread running\n");

    /* Diagnostic counters — print a summary every 5 s. */
    uint32_t stat_frames = 0;
    uint32_t stat_bytes = 0;
    uint32_t stat_eio = 0;
    uint32_t stat_last_ms = nano_get_millis();

    while (!g_audio.quit) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, pcm_buf, (snd_pcm_uframes_t)nsamples);
        if (n == -EINTR) {
            if (g_audio.quit) break;
            continue;
        }
        if (n == -EPIPE) {
            fprintf(stderr, "[audio] xrun, recovering\n");
            snd_pcm_prepare(pcm);
            snd_pcm_start(pcm);
            continue;
        }
        if (n == -ESTRPIPE) {
            int rc;
            while ((rc = snd_pcm_resume(pcm)) == -EAGAIN) {
                if (g_audio.quit) break;
                usleep(100 * 1000);
            }
            if (rc < 0) {
                snd_pcm_prepare(pcm);
                snd_pcm_start(pcm);
            }
            continue;
        }
        if (n < 0) {
            stat_eio++;
            /* Recover by prepare+start. If that doesn't help, close and
             * reopen the device entirely. */
            int rc = snd_pcm_recover(pcm, (int)n, 1);
            if (rc < 0) {
                fprintf(stderr, "[audio] unrecoverable error %s — reopening\n",
                        snd_strerror((int)n));
                snd_pcm_close(pcm);
                pcm = alsa_open(g_audio.cfg.device, rate, (unsigned int)ch,
                                 (snd_pcm_uframes_t)nsamples);
                if (!pcm) {
                    fprintf(stderr, "[audio] reopen failed — exiting thread\n");
                    break;
                }
            }
            usleep(5 * 1000);
            continue;
        }
        if (n < nsamples) continue;

        uint32_t pts = nano_get_millis();

        int olen = opus_encode(enc, pcm_buf, nsamples, opus_buf, (int)sizeof(opus_buf));
        if (olen < 0) {
            fprintf(stderr, "[audio] opus_encode: %s\n", opus_strerror(olen));
            continue;
        }
        if (olen == 0) continue;

        if (g_audio.cfg.callback) {
            g_audio.cfg.callback(g_audio.cfg.userdata, opus_buf, (size_t)olen, pts);
        }

        stat_frames++;
        stat_bytes += (uint32_t)olen;
        uint32_t now = nano_get_millis();
        if (now - stat_last_ms >= 5000) {
            uint32_t dt = now - stat_last_ms;
            uint32_t avg = stat_frames ? stat_bytes / stat_frames : 0;
            uint32_t kbps = dt ? (uint32_t)(((uint64_t)stat_bytes * 8) / dt) : 0;
            fprintf(stderr, "[audio] enc %u frames, avg %u B, ~%u kbps, eio=%u\n",
                    stat_frames, avg, kbps, stat_eio);
            stat_frames = 0;
            stat_bytes = 0;
            stat_eio = 0;
            stat_last_ms = now;
        }
    }

    free(pcm_buf);

cleanup:
    if (enc) opus_encoder_destroy(enc);
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    fprintf(stderr, "[audio] capture thread exiting\n");
    return NULL;
}

int audio_start(const audio_config_t *cfg)
{
    if (g_audio.running) return 0;
    if (!cfg || !cfg->device || cfg->sample_rate <= 0 || cfg->channels <= 0 ||
        cfg->frame_ms <= 0 || !cfg->callback) {
        fprintf(stderr, "[audio] invalid config\n");
        return -1;
    }

    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.cfg = *cfg;
    g_audio.frame_samples = cfg->sample_rate * cfg->frame_ms / 1000;
    atomic_store(&g_audio.init_state, 0);

    /* Install SIGUSR1 handler once per process. */
    static bool handler_installed = false;
    if (!handler_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = audio_sig_handler;
        /* Deliberately no SA_RESTART — we want snd_pcm_readi to see EINTR. */
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR1, &sa, NULL) != 0) {
            fprintf(stderr, "[audio] sigaction(SIGUSR1): %s\n", strerror(errno));
            return -1;
        }
        handler_installed = true;
    }

    /* Spawn capture thread. It does ALSA open + opus init + read loop. */
    g_audio.quit = 0;
    if (pthread_create(&g_audio.tid, NULL, audio_thread_fn, NULL) != 0) {
        fprintf(stderr, "[audio] pthread_create failed\n");
        return -1;
    }

    /* Wait for the thread to finish initialization (success or failure)
     * so audio_start can return a meaningful status. Time-bounded. */
    uint32_t t0 = nano_get_millis();
    while (atomic_load(&g_audio.init_state) == 0) {
        uint32_t now = nano_get_millis();
        if (now - t0 > 3000) {
            fprintf(stderr, "[audio] init timeout\n");
            g_audio.quit = 1;
            pthread_kill(g_audio.tid, SIGUSR1);
            pthread_join(g_audio.tid, NULL);
            return -1;
        }
        usleep(10 * 1000);
    }

    if (atomic_load(&g_audio.init_state) != 1) {
        pthread_join(g_audio.tid, NULL);
        return -1;
    }

    g_audio.running = true;
    return 0;
}

void audio_stop(void)
{
    if (!g_audio.running) return;

    g_audio.quit = 1;
    pthread_kill(g_audio.tid, SIGUSR1);
    pthread_join(g_audio.tid, NULL);

    g_audio.running = false;
    fprintf(stderr, "[audio] stopped\n");
}

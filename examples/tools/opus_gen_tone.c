/*
 * opus_gen_tone — Generate Opus-encoded test tone frames
 *
 * Creates sample-NNN.opus files containing a 440Hz sine wave
 * encoded in mono Opus at 48kHz, 20ms per frame.
 *
 * Usage: opus_gen_tone <output_dir> [num_frames]
 *
 * SPDX-License-Identifier: MIT
 */

#include <opus.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define CHANNELS    1       /* mono — matches common WebRTC usage */
#define FRAME_MS    20
#define FRAME_SIZE  (SAMPLE_RATE * FRAME_MS / 1000) /* 960 samples */
#define MAX_PACKET  4000
#define TONE_HZ     440.0

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <output_dir> [num_frames]\n", argv[0]);
        return 1;
    }

    const char *dir = argv[1];
    int num_frames = argc > 2 ? atoi(argv[2]) : 619;

    int err;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK) {
        fprintf(stderr, "opus_encoder_create: %s\n", opus_strerror(err));
        return 1;
    }

    /* Set bitrate to match sample data (~64kbps) */
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    /* Enable in-band FEC to match Chrome expectations */
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));

    int16_t pcm[FRAME_SIZE * CHANNELS];
    uint8_t opus_buf[MAX_PACKET];
    double phase = 0.0;
    double phase_inc = 2.0 * 3.14159265358979 * TONE_HZ / SAMPLE_RATE;

    int total_bytes = 0;

    for (int i = 0; i < num_frames; i++) {
        /* Generate sine wave */
        for (int s = 0; s < FRAME_SIZE; s++) {
            pcm[s] = (int16_t)(sin(phase) * 16000.0);
            phase += phase_inc;
            if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        }

        int nbytes = opus_encode(enc, pcm, FRAME_SIZE, opus_buf, MAX_PACKET);
        if (nbytes < 0) {
            fprintf(stderr, "Frame %d: encode error: %s\n", i, opus_strerror(nbytes));
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/sample-%03d.opus", dir, i);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(opus_buf, 1, (size_t)nbytes, f);
            fclose(f);
        }

        total_bytes += nbytes;

        if (i < 3 || i == num_frames - 1) {
            fprintf(stderr, "Frame %d: %d bytes, TOC=0x%02X\n", i, nbytes, opus_buf[0]);
        }
    }

    opus_encoder_destroy(enc);
    fprintf(stderr, "\nGenerated %d frames (avg %d bytes/frame) in %s\n",
            num_frames, total_bytes / num_frames, dir);
    return 0;
}

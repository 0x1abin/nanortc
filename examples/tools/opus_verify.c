/*
 * opus_verify — Decode raw Opus frames to PCM for local playback verification
 *
 * Usage: opus_verify <frame_dir> [output.raw]
 *   Reads sample-000.opus .. sample-NNN.opus from <frame_dir>
 *   Decodes each frame with libopus and writes s16le PCM to output file.
 *
 * Play result:
 *   ffplay -f s16le -ar 48000 -ac 2 /tmp/opus_decoded.raw
 *
 * SPDX-License-Identifier: MIT
 */

#include <opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_SIZE 5760 /* 120ms at 48kHz */
#define MAX_CHANNELS   2
#define MAX_FILE_SIZE  4096

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <frame_dir> [output.raw]\n", argv[0]);
        fprintf(stderr, "  Decodes Opus frames to s16le PCM (48kHz stereo)\n");
        fprintf(stderr, "  Play: ffplay -f s16le -ar 48000 -ac 2 output.raw\n");
        return 1;
    }

    const char *frame_dir = argv[1];
    const char *output_path = argc > 2 ? argv[2] : "/tmp/opus_decoded.raw";

    /* Peek at first frame TOC byte to detect stereo */
    char path[512];
    snprintf(path, sizeof(path), "%s/sample-000.opus", frame_dir);
    FILE *peek = fopen(path, "rb");
    if (!peek) {
        fprintf(stderr, "Cannot open %s\n", path);
        return 1;
    }
    unsigned char toc;
    if (fread(&toc, 1, 1, peek) != 1) {
        fprintf(stderr, "Empty frame file\n");
        fclose(peek);
        return 1;
    }
    fclose(peek);

    int stereo = (toc >> 2) & 1;
    int channels = stereo ? 2 : 1;
    fprintf(stderr, "TOC=0x%02X → %s\n", toc, stereo ? "stereo" : "mono");

    /* Create decoder */
    int err;
    OpusDecoder *dec = opus_decoder_create(48000, channels, &err);
    if (err != OPUS_OK) {
        fprintf(stderr, "opus_decoder_create failed: %s\n", opus_strerror(err));
        return 1;
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Cannot create %s\n", output_path);
        opus_decoder_destroy(dec);
        return 1;
    }

    int16_t pcm[MAX_FRAME_SIZE * MAX_CHANNELS];
    unsigned char frame_buf[MAX_FILE_SIZE];
    int total_frames = 0;
    int decode_errors = 0;
    int total_samples = 0;

    for (int i = 0; i < 10000; i++) {
        snprintf(path, sizeof(path), "%s/sample-%03d.opus", frame_dir, i);
        FILE *f = fopen(path, "rb");
        if (!f) break;

        size_t frame_len = fread(frame_buf, 1, sizeof(frame_buf), f);
        fclose(f);

        if (frame_len == 0) {
            decode_errors++;
            total_frames++;
            continue;
        }

        int samples = opus_decode(dec, frame_buf, (int)frame_len,
                                  pcm, MAX_FRAME_SIZE, 0);
        if (samples < 0) {
            fprintf(stderr, "Frame %d: decode error: %s\n", i, opus_strerror(samples));
            decode_errors++;
        } else {
            fwrite(pcm, sizeof(int16_t) * channels, (size_t)samples, out);
            total_samples += samples;
        }
        total_frames++;
    }

    fclose(out);
    opus_decoder_destroy(dec);

    double duration = (double)total_samples / 48000.0;
    fprintf(stderr, "\nDecoded %d frames → %d samples (%.1fs), %d errors\n",
            total_frames, total_samples, duration, decode_errors);
    fprintf(stderr, "Output: %s (%s, 48kHz, s16le)\n",
            output_path, stereo ? "stereo" : "mono");
    fprintf(stderr, "Play:   ffplay -f s16le -ar 48000 -ac %d %s\n",
            channels, output_path);

    return decode_errors > 0 ? 1 : 0;
}

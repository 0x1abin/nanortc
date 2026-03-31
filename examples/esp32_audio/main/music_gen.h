/*
 * music_gen.h — Three-voice algorithmic music generator
 *
 * Generates "Twinkle, Twinkle, Little Star" with melody (band-limited
 * square), bass (sine), and arpeggio (triangle) voices.  Outputs 16-bit
 * PCM suitable for any sample rate (8 kHz / 48 kHz).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MUSIC_GEN_H
#define MUSIC_GEN_H

#include <stddef.h>
#include <stdint.h>

/**
 * Fill @p pcm with algorithmically generated music.
 *
 * State is kept internally; consecutive calls produce a continuous,
 * seamlessly looping melody.  Call music_reset() to restart from the
 * beginning.
 *
 * @param pcm         Output buffer (interleaved if channels > 1).
 * @param samples     Number of samples (per channel) to generate.
 * @param sample_rate Sample rate in Hz (e.g. 8000 or 48000).
 * @param channels    Number of audio channels (typically 1).
 */
void music_generate(int16_t *pcm, size_t samples, int sample_rate,
                    int channels);

/** Reset the generator to the beginning of the melody. */
void music_reset(void);

#endif /* MUSIC_GEN_H */

/*
 * music_gen.c — Three-voice algorithmic music generator
 *
 * Voice 1 — Melody:   band-limited square wave (sin + sin*3/3)
 * Voice 2 — Bass:     pure sine, chord root
 * Voice 3 — Arpeggio: triangle wave, Alberti-bass pattern
 *
 * SPDX-License-Identifier: MIT
 */

#include "music_gen.h"

#include <math.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Tempo
 * ---------------------------------------------------------------- */
#ifndef CONFIG_EXAMPLE_MUSIC_BPM
#define CONFIG_EXAMPLE_MUSIC_BPM 120
#endif
#define MUSIC_BPM CONFIG_EXAMPLE_MUSIC_BPM

/* ----------------------------------------------------------------
 * Music data tables
 * ---------------------------------------------------------------- */
typedef struct {
    uint8_t note;     /* MIDI note number */
    uint8_t duration; /* quarter-note ticks */
} music_note_t;

/* Twinkle, Twinkle, Little Star — C major (42 notes, 48 ticks) */
static const music_note_t MELODY[] = {
    {60,1},{60,1},{67,1},{67,1},{69,1},{69,1},{67,2},
    {65,1},{65,1},{64,1},{64,1},{62,1},{62,1},{60,2},
    {67,1},{67,1},{65,1},{65,1},{64,1},{64,1},{62,2},
    {67,1},{67,1},{65,1},{65,1},{64,1},{64,1},{62,2},
    {60,1},{60,1},{67,1},{67,1},{69,1},{69,1},{67,2},
    {65,1},{65,1},{64,1},{64,1},{62,1},{62,1},{60,2},
};
#define MELODY_LEN (sizeof(MELODY) / sizeof(MELODY[0]))

/* Chord progression — one per phrase (8 ticks each) */
typedef struct {
    uint8_t bass;   /* bass MIDI note */
    uint8_t arp[3]; /* arpeggio chord tones */
} chord_entry_t;

static const chord_entry_t CHORDS[] = {
    {48, {60, 64, 67}}, /* C major  (bass C3) */
    {53, {65, 69, 72}}, /* F major  (bass F3) */
    {48, {60, 64, 67}}, /* C major            */
    {55, {59, 62, 67}}, /* G major  (bass G3) */
    {48, {60, 64, 67}}, /* C major            */
    {53, {65, 69, 72}}, /* F major            */
};
#define CHORDS_LEN      (sizeof(CHORDS) / sizeof(CHORDS[0]))
#define TICKS_PER_CHORD 8

/* Alberti-bass arpeggio pattern (indices into chord_entry_t.arp[]) */
static const uint8_t ARP_PATTERN[] = {0, 2, 1, 2};
#define ARP_PATTERN_LEN (sizeof(ARP_PATTERN) / sizeof(ARP_PATTERN[0]))

/* ----------------------------------------------------------------
 * Generator state
 * ---------------------------------------------------------------- */
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_OFF };

typedef struct {
    int      melody_idx;
    int      chord_idx;
    int      arp_step;

    uint32_t melody_note_pos;
    uint32_t melody_note_len;
    uint32_t bass_note_pos;
    uint32_t bass_note_len;
    uint32_t arp_note_pos;
    uint32_t arp_note_len;

    float    melody_phase;
    float    bass_phase;
    float    arp_phase;
    float    melody_phase_inc;
    float    bass_phase_inc;
    float    arp_phase_inc;

    float    melody_env;
    float    bass_env;
    float    arp_env;
    int      melody_env_state;
    int      bass_env_state;
    int      arp_env_state;

    int      initialized;
} music_state_t;

static music_state_t s_music;

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */
static float midi_to_phase_inc(uint8_t note, int sample_rate)
{
    float freq = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
    return 2.0f * (float)M_PI * freq / (float)sample_rate;
}

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float triangle_wave(float phase)
{
    float t = phase * (0.5f / (float)M_PI);
    t = t - (float)(int)t;
    if (t < 0.0f) t += 1.0f;
    return 4.0f * fabsf(t - 0.5f) - 1.0f;
}

/* Per-sample ADSR: attack 5ms, decay 20ms, sustain 0.7, release 15ms */
static inline void env_step(float *level, int *state,
                            uint32_t note_pos, uint32_t note_len,
                            uint32_t atk, uint32_t dec, uint32_t rel)
{
    if (*state < ENV_RELEASE && note_len > rel &&
        note_pos >= note_len - rel) {
        *state = ENV_RELEASE;
    }

    switch (*state) {
    case ENV_ATTACK:
        *level += 1.0f / (float)(atk > 0 ? atk : 1);
        if (*level >= 1.0f) { *level = 1.0f; *state = ENV_DECAY; }
        break;
    case ENV_DECAY:
        *level -= 0.3f / (float)(dec > 0 ? dec : 1);
        if (*level <= 0.7f) { *level = 0.7f; *state = ENV_SUSTAIN; }
        break;
    case ENV_SUSTAIN:
        break;
    case ENV_RELEASE:
        *level -= 0.7f / (float)(rel > 0 ? rel : 1);
        if (*level <= 0.0f) { *level = 0.0f; *state = ENV_OFF; }
        break;
    default:
        *level = 0.0f;
        break;
    }
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */
void music_reset(void)
{
    memset(&s_music, 0, sizeof(s_music));
}

void music_generate(int16_t *pcm, size_t samples, int sample_rate,
                    int channels)
{
    const float two_pi = 2.0f * (float)M_PI;
    const uint32_t spt = (uint32_t)(sample_rate * 60 / MUSIC_BPM);

    /* Envelope timing (samples) */
    const uint32_t atk = (uint32_t)(sample_rate *  5 / 1000);
    const uint32_t dec = (uint32_t)(sample_rate * 20 / 1000);
    const uint32_t rel = (uint32_t)(sample_rate * 15 / 1000);
    const uint32_t arp_len = spt / 2; /* eighth note */

    if (!s_music.initialized) {
        s_music.melody_phase_inc =
            midi_to_phase_inc(MELODY[0].note, sample_rate);
        s_music.melody_note_len = MELODY[0].duration * spt;
        s_music.melody_env_state = ENV_ATTACK;

        s_music.bass_phase_inc =
            midi_to_phase_inc(CHORDS[0].bass, sample_rate);
        s_music.bass_note_len = TICKS_PER_CHORD * spt;
        s_music.bass_env_state = ENV_ATTACK;

        s_music.arp_phase_inc = midi_to_phase_inc(
            CHORDS[0].arp[ARP_PATTERN[0]], sample_rate);
        s_music.arp_note_len = arp_len;
        s_music.arp_env_state = ENV_ATTACK;

        s_music.initialized = 1;
    }

    for (size_t i = 0; i < samples; i++) {
        /* --- Melody note boundary --- */
        if (s_music.melody_note_pos >= s_music.melody_note_len) {
            s_music.melody_note_pos = 0;
            s_music.melody_idx =
                (s_music.melody_idx + 1) % (int)MELODY_LEN;
            s_music.melody_phase_inc = midi_to_phase_inc(
                MELODY[s_music.melody_idx].note, sample_rate);
            s_music.melody_note_len =
                MELODY[s_music.melody_idx].duration * spt;
            s_music.melody_env = 0.0f;
            s_music.melody_env_state = ENV_ATTACK;
        }

        /* --- Chord boundary --- */
        if (s_music.bass_note_pos >= s_music.bass_note_len) {
            s_music.bass_note_pos = 0;
            s_music.chord_idx =
                (s_music.chord_idx + 1) % (int)CHORDS_LEN;
            s_music.bass_phase_inc = midi_to_phase_inc(
                CHORDS[s_music.chord_idx].bass, sample_rate);
            s_music.bass_note_len = TICKS_PER_CHORD * spt;
            s_music.bass_env = 0.0f;
            s_music.bass_env_state = ENV_ATTACK;
            s_music.arp_step = 0;
            s_music.arp_note_pos = 0;
            s_music.arp_phase_inc = midi_to_phase_inc(
                CHORDS[s_music.chord_idx].arp[ARP_PATTERN[0]],
                sample_rate);
            s_music.arp_note_len = arp_len;
            s_music.arp_env = 0.0f;
            s_music.arp_env_state = ENV_ATTACK;
        }

        /* --- Arpeggio step boundary --- */
        if (s_music.arp_note_pos >= s_music.arp_note_len) {
            s_music.arp_note_pos = 0;
            s_music.arp_step =
                (s_music.arp_step + 1) % (int)ARP_PATTERN_LEN;
            s_music.arp_phase_inc = midi_to_phase_inc(
                CHORDS[s_music.chord_idx]
                    .arp[ARP_PATTERN[s_music.arp_step]],
                sample_rate);
            s_music.arp_note_len = arp_len;
            s_music.arp_env = 0.0f;
            s_music.arp_env_state = ENV_ATTACK;
        }

        /* --- Envelopes --- */
        env_step(&s_music.melody_env, &s_music.melody_env_state,
                 s_music.melody_note_pos, s_music.melody_note_len,
                 atk, dec, rel);
        env_step(&s_music.bass_env, &s_music.bass_env_state,
                 s_music.bass_note_pos, s_music.bass_note_len,
                 atk, dec, rel);
        env_step(&s_music.arp_env, &s_music.arp_env_state,
                 s_music.arp_note_pos, s_music.arp_note_len,
                 atk, dec, rel);

        /* --- Waveforms --- */
        float mel = (sinf(s_music.melody_phase) +
                     0.33f * sinf(3.0f * s_music.melody_phase)) *
                    s_music.melody_env * 8000.0f;

        float bas = sinf(s_music.bass_phase) *
                    s_music.bass_env * 5000.0f;

        float arp = triangle_wave(s_music.arp_phase) *
                    s_music.arp_env * 3000.0f;

        /* --- Mix and output --- */
        int16_t val =
            (int16_t)clampf(mel + bas + arp, -32000.0f, 32000.0f);
        for (int ch = 0; ch < channels; ch++) {
            pcm[i * channels + ch] = val;
        }

        /* --- Advance oscillators --- */
        s_music.melody_phase += s_music.melody_phase_inc;
        if (s_music.melody_phase >= two_pi)
            s_music.melody_phase -= two_pi;
        s_music.bass_phase += s_music.bass_phase_inc;
        if (s_music.bass_phase >= two_pi)
            s_music.bass_phase -= two_pi;
        s_music.arp_phase += s_music.arp_phase_inc;
        if (s_music.arp_phase >= two_pi)
            s_music.arp_phase -= two_pi;

        s_music.melody_note_pos++;
        s_music.bass_note_pos++;
        s_music.arp_note_pos++;
    }
}

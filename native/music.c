#include "music.h"

#include <string.h>

#include "audio.h"

enum {
    CHANNELS = 3,
    ENVELOPE = 0x10c,  // $500C in the decoded $4F00 image
    PERIODS = 0x113,   // $5013
    FIRST_PITCH = 12,
    TRACK_A = 0x1d3,   // $50D3
    TRACK_B = 0x255,   // $5155
    TRACK_C = 0x397,   // $5297
    SCORE_SIZE = 1051,
};

static const unsigned track_start[CHANNELS] = {TRACK_A, TRACK_B, TRACK_C};
static const unsigned track_end[CHANNELS] = {TRACK_B, TRACK_C, SCORE_SIZE};
static const unsigned starting_volume[CHANNELS] = {15, 12, 15};

struct music_voice {
    unsigned position;
    unsigned note_time;
    unsigned volume;
    unsigned envelope_section;
    unsigned envelope_step;
    unsigned envelope_time;
};

static uint8_t score[SCORE_SIZE];
static struct music_voice voices[CHANNELS];
static bool loaded;
static bool playing;

static unsigned pitch_period(const uint8_t *image, unsigned pitch)
{
    if (pitch < FIRST_PITCH) {
        return 0;
    }
    const unsigned offset = PERIODS + 2 * (pitch - FIRST_PITCH);
    if (offset + 1 >= TRACK_A) {
        return 0;
    }
    return image[offset] | ((unsigned)image[offset + 1] << 8);
}

static bool valid_track(const uint8_t *image, unsigned channel)
{
    const unsigned start = track_start[channel];
    const unsigned end = track_end[channel];
    for (unsigned pos = start; pos + 1 < end; pos += 2) {
        if (image[pos] == 0xff) {
            return pos != start;
        }
        unsigned period = pitch_period(image, image[pos]);
        if (!period || period > 0xfff || !image[pos + 1]) {
            return false;
        }
    }
    return false;
}

bool music_load(const uint8_t *decoded, size_t size)
{
    if (!decoded || size != SCORE_SIZE || !decoded[ENVELOPE] ||
        ENVELOPE + 1 + 3 * decoded[ENVELOPE] > PERIODS) {
        return false;
    }
    for (unsigned section = 0; section < decoded[ENVELOPE]; ++section) {
        if (decoded[ENVELOPE + 1 + 3 * section] >= 0x80) {
            return false; // The title tune uses software volume steps only.
        }
    }
    for (unsigned channel = 0; channel < CHANNELS; ++channel) {
        if (!valid_track(decoded, channel)) {
            return false;
        }
    }
    music_stop();
    memcpy(score, decoded, sizeof(score));
    loaded = true;
    return true;
}

void music_start(void)
{
    if (!loaded || playing) {
        return;
    }
    for (unsigned channel = 0; channel < CHANNELS; ++channel) {
        voices[channel] = (struct music_voice){.position = track_start[channel]};
    }
    audio_music_reset();
    audio_music_register(7, 0x38); // Tone on all three channels, noise off.
    playing = true;
}

void music_stop(void)
{
    if (playing) {
        playing = false;
        audio_music_reset();
    }
}

static void start_note(unsigned channel, struct music_voice *voice)
{
    if (score[voice->position] == 0xff) {
        voice->position = track_start[channel];
    }
    const unsigned period = pitch_period(score, score[voice->position]);
    voice->note_time = score[voice->position + 1];
    voice->position += 2;
    voice->volume = starting_volume[channel];
    voice->envelope_section = 0;
    voice->envelope_step = 0;
    voice->envelope_time = 0;
    audio_music_register(2 * channel, (uint8_t)period);
    audio_music_register(2 * channel + 1, (uint8_t)(period >> 8));
}

static void volume_step(unsigned channel, struct music_voice *voice)
{
    if (!voice->envelope_time && voice->envelope_section < score[ENVELOPE]) {
        const uint8_t *section = score + ENVELOPE + 1 + 3 * voice->envelope_section;
        if (section[0]) {
            voice->volume = (unsigned)((int)voice->volume + (int8_t)section[1]) & 15;
        } else {
            voice->volume = section[1] & 15;
        }
        audio_music_register(8 + channel, (uint8_t)voice->volume);
        if (++voice->envelope_step >= section[0]) {
            ++voice->envelope_section;
            voice->envelope_step = 0;
        }
        voice->envelope_time = section[2] ? section[2] : 256;
    }
    if (voice->envelope_time) {
        --voice->envelope_time;
    }
}

bool music_half_tick(void)
{
    if (!playing) {
        return false;
    }
    // The original firmware measures SOUND duration and envelope steps in 10 ms ticks.
    for (unsigned channel = 0; channel < CHANNELS; ++channel) {
        struct music_voice *voice = &voices[channel];
        if (!voice->note_time) {
            start_note(channel, voice);
        }
        volume_step(channel, voice);
        --voice->note_time;
    }
    return true;
}

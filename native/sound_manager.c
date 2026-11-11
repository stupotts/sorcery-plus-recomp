#include "sound_manager.h"

#include <string.h>

#include "audio.h"

enum {
    CHANNELS = 3,
    CHANNEL_MASK = 7,
    QUEUE_SIZE = 4,
    ENVELOPE_COUNT = 16,
    ENVELOPE_SECTIONS = 5,
    SOUND_HOLD = 0x40,
    SOUND_FLUSH = 0x80,
};

struct envelope {
    uint8_t count;
    bool repeat;
    uint8_t section[ENVELOPE_SECTIONS][3];
};

struct envelope_cursor {
    const struct envelope *definition;
    unsigned section;
    unsigned steps;
    unsigned pause;
    bool repeat;
};

struct sound_command {
    uint8_t channels;
    uint8_t volume_envelope;
    uint8_t tone_envelope;
    uint8_t volume;
    uint8_t noise;
    uint16_t period;
    uint16_t duration;
};

struct sound_channel {
    struct sound_command queue[QUEUE_SIZE];
    unsigned head;
    unsigned count;
    unsigned remaining;
    unsigned volume;
    unsigned period;
    unsigned noise;
    struct envelope_cursor volume_cursor;
    struct envelope_cursor tone_cursor;
};

static struct sound_channel channels[CHANNELS];
static struct envelope volumes[ENVELOPE_COUNT];
static struct envelope tones[ENVELOPE_COUNT];

static unsigned little_word(const uint8_t *data)
{
    return data[0] | ((unsigned)data[1] << 8);
}

static unsigned step_pause(uint8_t value)
{
    return value ? value : 256;
}

static unsigned envelope_duration(const struct envelope *envelope)
{
    if (!envelope->count) {
        return 200;
    }
    unsigned duration = 0;
    for (unsigned i = 0; i < envelope->count; ++i) {
        const uint8_t *section = envelope->section[i];
        if (!(section[0] & 0x80)) {
            unsigned steps = section[0] ? section[0] : 1;
            duration += steps * step_pause(section[2]);
        }
    }
    return duration ? duration : 1;
}

static void write_mixer(void)
{
    unsigned mixer = 0x3f;
    for (unsigned i = 0; i < CHANNELS; ++i) {
        const struct sound_channel *channel = &channels[i];
        if (channel->remaining && channel->period) {
            mixer &= ~(1u << i);
        }
        if (channel->remaining && channel->noise) {
            mixer &= ~(8u << i);
        }
    }
    audio_register(7, (uint8_t)mixer);
}

static void write_tone(unsigned index)
{
    const unsigned period = channels[index].period;
    audio_register(2 * index, (uint8_t)period);
    audio_register(2 * index + 1, (uint8_t)(period >> 8));
}

static bool next_envelope_step(struct envelope_cursor *cursor, bool tone, unsigned *value)
{
    if (!cursor->definition->count || cursor->pause) {
        return false;
    }
    if (cursor->section == cursor->definition->count && cursor->repeat) {
        cursor->section = 0;
    }

    // A hardware volume section changes the PSG envelope immediately and takes no time.
    for (unsigned visited = 0; cursor->section < cursor->definition->count &&
         visited < ENVELOPE_SECTIONS; ++visited) {
        const uint8_t *section = cursor->definition->section[cursor->section];
        const unsigned kind = section[0];
        if (!tone && (kind & 0x80)) {
            audio_register(11, section[1]);
            audio_register(12, section[2]);
            audio_register(13, (uint8_t)(kind & 15));
            *value = (*value & 15) | 16;
            ++cursor->section;
            continue;
        }

        if (tone && kind >= 0xf0) {
            *value = ((kind & 15) << 8) | section[1];
        } else if (!tone && kind == 0) {
            *value = section[1] & 15;
        } else {
            const int delta = (int8_t)section[1];
            const unsigned mask = tone ? 0xfff : 15;
            // A volume step of zero can follow a hardware-envelope section as a delay.
            if (tone || delta || !(*value & 16)) {
                *value = (unsigned)((int)*value + delta) & mask;
            }
        }

        const unsigned count = tone && kind >= 0xf0 ? 1 : kind ? kind : 1;
        if (++cursor->steps >= count) {
            ++cursor->section;
            cursor->steps = 0;
        }
        cursor->pause = step_pause(section[2]);
        return true;
    }
    return !tone && (*value & 16);
}

static void advance_envelope(unsigned index, bool tone)
{
    struct sound_channel *channel = &channels[index];
    struct envelope_cursor *cursor = tone ? &channel->tone_cursor : &channel->volume_cursor;
    unsigned *value = tone ? &channel->period : &channel->volume;
    if (cursor->pause) {
        --cursor->pause;
    }
    const unsigned previous = *value;
    if (next_envelope_step(cursor, tone, value) && previous != *value) {
        if (tone) {
            write_tone(index);
            if (!previous || !*value) {
                write_mixer();
            }
        } else {
            audio_register(8 + index, (uint8_t)*value);
        }
    }
}

static void begin_sound(unsigned index)
{
    struct sound_channel *channel = &channels[index];
    const struct sound_command *command = &channel->queue[channel->head];
    const struct envelope *volume = &volumes[command->volume_envelope];
    const struct envelope *tone = &tones[command->tone_envelope];
    const bool envelope_time = command->duration == 0 || command->duration >= 0x8000;
    const unsigned repeat = command->duration >= 0x8000 ? 0x10000u - command->duration : 1;
    channel->remaining = envelope_time ? repeat * envelope_duration(volume) : command->duration;
    channel->volume = command->volume;
    channel->period = command->period;
    channel->noise = command->noise;
    channel->volume_cursor = (struct envelope_cursor){.definition = volume, .repeat = envelope_time};
    channel->tone_cursor = (struct envelope_cursor){.definition = tone, .repeat = tone->repeat};
    channel->head = (channel->head + 1) % QUEUE_SIZE;
    --channel->count;

    next_envelope_step(&channel->tone_cursor, true, &channel->period);
    next_envelope_step(&channel->volume_cursor, false, &channel->volume);
    write_tone(index);
    audio_register(8 + index, (uint8_t)channel->volume);
    if (channel->noise) {
        audio_register(6, (uint8_t)channel->noise);
    }
}

static void begin_ready(void)
{
    unsigned waiting_for[CHANNELS] = {0};
    unsigned ready = 0;
    for (unsigned i = 0; i < CHANNELS; ++i) {
        const struct sound_channel *channel = &channels[i];
        const unsigned bits = channel->queue[channel->head].channels;
        if (channel->count && !channel->remaining && !(bits & SOUND_HOLD)) {
            ready |= 1u << i;
            waiting_for[i] = ((bits | (bits >> 3)) & CHANNEL_MASK) & ~(1u << i);
        }
    }

    // Drop candidates with missing or incompatible rendezvous partners.
    unsigned previous;
    do {
        previous = ready;
        for (unsigned i = 0; i < CHANNELS; ++i) {
            for (unsigned j = 0; j < CHANNELS; ++j) {
                if ((waiting_for[i] & (1u << j)) &&
                    (!(ready & (1u << j)) || !(waiting_for[j] & (1u << i)))) {
                    ready &= ~(1u << i);
                }
            }
        }
    } while (ready != previous);

    for (unsigned i = 0; i < CHANNELS; ++i) {
        if (ready & (1u << i)) {
            begin_sound(i);
        }
    }
    if (ready) {
        write_mixer();
    }
}

void sound_reset(void)
{
    // BCA7 abandons queued sounds but leaves registered envelopes available.
    memset(channels, 0, sizeof(channels));
    for (unsigned i = 0; i < CHANNELS; ++i) {
        audio_register(8 + i, 0);
    }
    write_mixer();
}

bool sound_queue(const uint8_t data[9])
{
    const unsigned selected = data[0] & CHANNEL_MASK;
    if (!selected) {
        return true;
    }
    const bool flush = (data[0] & SOUND_FLUSH) != 0;
    if (!flush) {
        for (unsigned i = 0; i < CHANNELS; ++i) {
            if ((selected & (1u << i)) && channels[i].count == QUEUE_SIZE) {
                return false;
            }
        }
    }

    const struct sound_command command = {
        .channels = data[0],
        .volume_envelope = data[1] & 15,
        .tone_envelope = data[2] & 15,
        .volume = data[6] & 15,
        .noise = data[5] & 31,
        .period = (uint16_t)(little_word(data + 3) & 0xfff),
        .duration = (uint16_t)little_word(data + 7),
    };
    for (unsigned i = 0; i < CHANNELS; ++i) {
        if (!(selected & (1u << i))) {
            continue;
        }
        struct sound_channel *channel = &channels[i];
        if (flush) {
            channel->count = 0;
            channel->remaining = 0;
            audio_register(8 + i, 0);
        }
        channel->queue[(channel->head + channel->count) % QUEUE_SIZE] = command;
        ++channel->count;
    }
    if (flush) {
        write_mixer();
    }
    begin_ready();
    return true;
}

uint8_t sound_status(uint8_t mask)
{
    for (unsigned i = 0; i < CHANNELS; ++i) {
        if (!(mask & (1u << i))) {
            continue;
        }
        const struct sound_channel *channel = &channels[i];
        unsigned status = QUEUE_SIZE - channel->count;
        if (channel->remaining) {
            status |= 0x80;
        } else if (channel->count) {
            const unsigned bits = channel->queue[channel->head].channels;
            const unsigned partners = ((bits | (bits >> 3)) & CHANNEL_MASK) & ~(1u << i);
            status |= bits & SOUND_HOLD ? SOUND_HOLD : partners << 3;
        }
        return (uint8_t)status;
    }
    return 0;
}

void sound_envelope(bool tone, unsigned index, const uint8_t data[16])
{
    if (!index || index >= ENVELOPE_COUNT) {
        return;
    }
    struct envelope *envelope = &(tone ? tones : volumes)[index];
    const uint8_t count = data[0] & 0x7f;
    envelope->count = count > ENVELOPE_SECTIONS ? ENVELOPE_SECTIONS : count;
    envelope->repeat = tone && (data[0] & 0x80);
    memcpy(envelope->section, data + 1, sizeof(envelope->section));
}

void sound_frame(void)
{
    for (unsigned half = 0; half < 2; ++half) {
        audio_half_tick();
        bool finished = false;
        for (unsigned i = 0; i < CHANNELS; ++i) {
            if (!channels[i].remaining) {
                continue;
            }
            if (--channels[i].remaining) {
                advance_envelope(i, true);
                advance_envelope(i, false);
            } else {
                audio_register(8 + i, 0);
                finished = true;
            }
        }
        if (finished) {
            write_mixer();
            begin_ready();
        }
    }
}

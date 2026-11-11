#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#include "audio.h"
#include "music.h"

enum {
    PSG_TICKS_PER_SECOND = 1000000 / 8,
    SAMPLE_RATE = 48000,
    SAMPLES_PER_FRAME = SAMPLE_RATE / 50,
    SAMPLES_PER_HALF_FRAME = SAMPLES_PER_FRAME / 2,
    OUTPUT_CHANNELS = 2,
    OUTPUT_BUFFERS = 4,
};

struct tone {
    unsigned counter;
    bool high;
};

struct psg {
    uint8_t registers[14];
    struct tone tones[3];
    unsigned noise_counter;
    uint32_t noise_bits;
    unsigned envelope_counter;
    int envelope_level;
    int envelope_direction;
    int64_t dc_offset[OUTPUT_CHANNELS];
};

static struct psg primary_psg = {.noise_bits = 0x1ffff};
static struct psg music_psg = {.noise_bits = 0x1ffff};
static unsigned sample_clock;
static unsigned sfx_level = 100;
static unsigned music_level = 50;
static bool title_music;
static bool gameplay_music = true;

static HWAVEOUT device;
static WAVEHDR headers[OUTPUT_BUFFERS];
static int16_t samples[OUTPUT_BUFFERS][SAMPLES_PER_FRAME * OUTPUT_CHANNELS];
static int16_t pending_samples[SAMPLES_PER_FRAME * OUTPUT_CHANNELS];
static unsigned pending_frames;
static unsigned prepared_buffers;
static unsigned next_buffer;

// Approximate the AY's logarithmic DAC. The mix leaves headroom for stereo output.
static const int amplitude[16] = {
    0, 69, 103, 153, 228, 339, 504, 750,
    1115, 1658, 2466, 3667, 5453, 8108, 12059, 18000,
};

static unsigned period_or_one(unsigned period)
{
    return period ? period : 1;
}

static void write_register(struct psg *psg, unsigned reg, uint8_t value)
{
    if (reg >= sizeof(psg->registers)) {
        return;
    }
    psg->registers[reg] = value;
    if (reg == 13) {
        psg->envelope_direction = (value & 4) ? 1 : -1;
        psg->envelope_level = psg->envelope_direction > 0 ? 0 : 15;
        psg->envelope_counter = 0;
    }
}

void audio_register(unsigned reg, uint8_t value)
{
    write_register(&primary_psg, reg, value);
}

void audio_music_register(unsigned reg, uint8_t value)
{
    write_register(&music_psg, reg, value);
}

void audio_music_reset(void)
{
    music_psg = (struct psg){.noise_bits = 0x1ffff};
}

void audio_set_levels(unsigned sfx, unsigned music, bool enable_gameplay_music)
{
    sfx_level = sfx > 100 ? 100 : sfx;
    music_level = music > 100 ? 100 : music;
    gameplay_music = enable_gameplay_music;
}

void audio_set_title_music(bool active)
{
    title_music = active;
}

static void advance_envelope(struct psg *psg)
{
    unsigned period = psg->registers[11] | ((unsigned)psg->registers[12] << 8);
    // Each AY envelope ramp covers 16 levels and takes 256 clocks per period.
    unsigned divider = period ? 2 * period : 1;
    if (++psg->envelope_counter < divider) {
        return;
    }
    psg->envelope_counter = 0;
    psg->envelope_level += psg->envelope_direction;
    if (psg->envelope_level >= 0 && psg->envelope_level <= 15) {
        return;
    }

    const unsigned shape = psg->registers[13];
    if (!(shape & 8)) {
        psg->envelope_level = 0;
        psg->envelope_direction = 0;
        return;
    }
    if (shape & 2) {
        psg->envelope_direction = -psg->envelope_direction;
    }
    if (shape & 1) {
        psg->envelope_level = psg->envelope_direction > 0 ? 15 : 0;
        psg->envelope_direction = 0;
    } else {
        psg->envelope_level = psg->envelope_direction > 0 ? 0 : 15;
    }
}

static void mix_psg_tick(struct psg *psg, int output[OUTPUT_CHANNELS])
{
    const unsigned mixer = psg->registers[7];
    const unsigned noise_period = 2 * period_or_one(psg->registers[6] & 31);
    if (++psg->noise_counter >= noise_period) {
        psg->noise_counter = 0;
        const uint32_t feedback = (psg->noise_bits ^ (psg->noise_bits >> 3)) & 1;
        psg->noise_bits = (psg->noise_bits >> 1) | (feedback << 16);
    }
    advance_envelope(psg);

    int voice[3];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned period = psg->registers[2 * i] | ((unsigned)(psg->registers[2 * i + 1] & 15) << 8);
        if (++psg->tones[i].counter >= period_or_one(period)) {
            psg->tones[i].counter = 0;
            psg->tones[i].high = !psg->tones[i].high;
        }

        const bool tone_gate = (mixer & (1u << i)) || psg->tones[i].high;
        const bool noise_gate = (mixer & (8u << i)) || (psg->noise_bits & 1);
        const unsigned level = psg->registers[8 + i];
        const int volume = (level & 16) ? psg->envelope_level : (int)(level & 15);
        voice[i] = tone_gate && noise_gate ? amplitude[volume] : 0;
    }
    // The CPC sends A to the left, C to the right, and B to both.
    output[0] += voice[0] + voice[1] / 2;
    output[1] += voice[2] + voice[1] / 2;
}

static int remove_dc(struct psg *psg, int sample, unsigned channel)
{
    int64_t *offset = &psg->dc_offset[channel];
    // A slow one-pole filter centres the unsigned DAC output around zero.
    *offset += ((int64_t)sample * 65536 - *offset) / 384;
    return (int)(sample - *offset / 65536);
}

static int16_t pcm_sample(int value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static void mix_samples(int16_t *output, unsigned count, bool background_active)
{
    const int primary_level = (int)(title_music ? music_level : sfx_level);
    const int background_level = gameplay_music ? (int)music_level : 0;
    for (unsigned i = 0; i < count; ++i) {
        int primary[OUTPUT_CHANNELS] = {0};
        int background[OUTPUT_CHANNELS] = {0};
        int ticks = 0;
        sample_clock += PSG_TICKS_PER_SECOND;
        while (sample_clock >= SAMPLE_RATE) {
            sample_clock -= SAMPLE_RATE;
            mix_psg_tick(&primary_psg, primary);
            if (background_active) {
                mix_psg_tick(&music_psg, background);
            }
            ++ticks;
        }

        // Average the chip ticks inside this sample before downsampling to PCM.
        for (unsigned channel = 0; channel < OUTPUT_CHANNELS; ++channel) {
            int sample = remove_dc(&primary_psg, primary[channel] / ticks, channel) * primary_level / 100;
            if (background_active) {
                sample += remove_dc(&music_psg, background[channel] / ticks, channel) * background_level / 100;
            }
            output[OUTPUT_CHANNELS * i + channel] = pcm_sample(sample);
        }
    }
}

bool audio_open(void)
{
    if (device) {
        return true;
    }

    const WAVEFORMATEX format = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = OUTPUT_CHANNELS,
        .nSamplesPerSec = SAMPLE_RATE,
        .nAvgBytesPerSec = SAMPLE_RATE * OUTPUT_CHANNELS * sizeof(int16_t),
        .nBlockAlign = OUTPUT_CHANNELS * sizeof(int16_t),
        .wBitsPerSample = 16,
    };
    if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        device = NULL;
        return false;
    }
    for (unsigned i = 0; i < OUTPUT_BUFFERS; ++i) {
        headers[i] = (WAVEHDR){.lpData = (LPSTR)samples[i], .dwBufferLength = sizeof(samples[i])};
        if (waveOutPrepareHeader(device, &headers[i], sizeof(headers[i])) != MMSYSERR_NOERROR) {
            audio_close();
            return false;
        }
        ++prepared_buffers;
    }
    next_buffer = 0;
    pending_frames = 0;
    return true;
}

static bool measure_pcm;
static uint64_t pcm_hash;
static unsigned submissions;

void audio_measure(void)
{
    measure_pcm = true;
    pcm_hash = 14695981039346656037ull;
}

uint64_t audio_pcm_hash(void) { return pcm_hash; }
unsigned audio_submissions(void) { return submissions; }

void audio_half_tick(void)
{
    const bool background_active = music_half_tick();
    if (!device && !measure_pcm) {
        return;
    }

    int16_t *half = pending_samples + pending_frames * OUTPUT_CHANNELS;
    mix_samples(half, SAMPLES_PER_HALF_FRAME, background_active);
    if (measure_pcm) {
        for (unsigned i = 0; i < SAMPLES_PER_HALF_FRAME * OUTPUT_CHANNELS; ++i) {
            uint16_t sample = (uint16_t)half[i];
            pcm_hash = (pcm_hash ^ (sample & 255)) * 1099511628211ull;
            pcm_hash = (pcm_hash ^ (sample >> 8)) * 1099511628211ull;
        }
    }
    pending_frames += SAMPLES_PER_HALF_FRAME;
    if (pending_frames < SAMPLES_PER_FRAME) {
        return;
    }
    pending_frames = 0;
    if (!device) return;

    WAVEHDR *header = &headers[next_buffer];
    if (header->dwFlags & WHDR_INQUEUE) {
        return;
    }

    memcpy(samples[next_buffer], pending_samples, sizeof(pending_samples));
    if (waveOutWrite(device, header, sizeof(*header)) == MMSYSERR_NOERROR) {
        ++submissions;
        next_buffer = (next_buffer + 1) % OUTPUT_BUFFERS;
    } else {
        audio_close();
    }
}

void audio_tick(void)
{
    audio_half_tick();
    audio_half_tick();
}

void audio_close(void)
{
    if (!device) {
        return;
    }

    waveOutReset(device);
    for (unsigned i = 0; i < prepared_buffers; ++i) {
        waveOutUnprepareHeader(device, &headers[i], sizeof(headers[i]));
    }
    waveOutClose(device);
    prepared_buffers = 0;
    device = NULL;
}

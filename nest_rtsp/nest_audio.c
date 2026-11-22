#include "nest_audio.h"

#include <errno.h>
#include <sound/asound.h>
#include <stdio.h>

/* PCM API exported by the firmware's libasound.so.2. */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
extern int snd_pcm_open(snd_pcm_t **, const char *, int, int);
extern int snd_pcm_close(snd_pcm_t *);
extern int snd_pcm_set_params(snd_pcm_t *, snd_pcm_format_t, snd_pcm_access_t,
                              unsigned, unsigned, int, unsigned);
extern int snd_pcm_start(snd_pcm_t *);
extern snd_pcm_state_t snd_pcm_state(snd_pcm_t *);
extern int snd_pcm_drop(snd_pcm_t *);
extern int snd_pcm_prepare(snd_pcm_t *);
extern int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **);
extern void snd_pcm_sw_params_free(snd_pcm_sw_params_t *);
extern int snd_pcm_sw_params_current(snd_pcm_t *, snd_pcm_sw_params_t *);
extern int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *, snd_pcm_sw_params_t *,
                                                 snd_pcm_uframes_t);
extern int snd_pcm_sw_params(snd_pcm_t *, snd_pcm_sw_params_t *);
extern snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *, void *, snd_pcm_uframes_t);
extern snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
extern const char *snd_strerror(int);

#define PCM_NONBLOCK 1
#define HW_RATE AUDIO_RTP_RATE
#define HW_CHANNELS 2
#define PCM_BUFFER_US 120000
#define PCM_START_FRAMES (HW_RATE * 60 / 1000)

static snd_pcm_t *capture;
static snd_pcm_t *playback;
static size_t playback_pending;

static snd_pcm_t *open_pcm(int direction) {
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, "hw:0,0", direction, PCM_NONBLOCK);
    if (err >= 0) {
        err = snd_pcm_set_params(pcm, SNDRV_PCM_FORMAT_S16_LE,
                                 SNDRV_PCM_ACCESS_RW_INTERLEAVED, HW_CHANNELS,
                                 HW_RATE, 0, PCM_BUFFER_US);
    }
    if (err >= 0 && direction == SNDRV_PCM_STREAM_CAPTURE) {
        err = snd_pcm_start(pcm);
    } else if (err >= 0) {
        snd_pcm_sw_params_t *params;
        err = snd_pcm_sw_params_malloc(&params);
        if (err >= 0) {
            err = snd_pcm_sw_params_current(pcm, params);
            if (err >= 0)
                err = snd_pcm_sw_params_set_start_threshold(pcm, params, PCM_START_FRAMES);
            if (err >= 0)
                err = snd_pcm_sw_params(pcm, params);
            snd_pcm_sw_params_free(params);
        }
    }
    if (err >= 0) return pcm;

    fprintf(stderr, "[audio] %s: %s\n",
            direction == SNDRV_PCM_STREAM_CAPTURE ? "capture" : "playback",
            snd_strerror(err));
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

int audio_open(int requested) {
    /* Both directions share I²S clocks. Set up playback before starting capture. */
    if (requested & AUDIO_PLAYBACK) playback = open_pcm(SNDRV_PCM_STREAM_PLAYBACK);
    if (requested & AUDIO_CAPTURE) capture = open_pcm(SNDRV_PCM_STREAM_CAPTURE);
    return (capture ? AUDIO_CAPTURE : 0) | (playback ? AUDIO_PLAYBACK : 0);
}

void audio_close(void) {
    if (capture) snd_pcm_close(capture);
    if (playback) snd_pcm_close(playback);
    capture = playback = NULL;
}

static uint8_t pcm_to_mulaw(int sample) {
    unsigned sign = sample < 0 ? 0x80 : 0;
    if (sample < 0) sample = -sample;
    if (sample > 32635) sample = 32635;
    unsigned magnitude = (unsigned)sample + 132;
    unsigned segment = 0;
    for (unsigned bits = magnitude >> 8; bits; bits >>= 1)
        segment++;
    return (uint8_t)~(sign | (segment << 4) | ((magnitude >> (segment + 3)) & 15));
}

static int16_t mulaw_to_pcm(uint8_t sample) {
    unsigned code = (uint8_t)~sample;
    int magnitude = (int)(((code & 15) * 8 + 132) << ((code >> 4) & 7)) - 132;
    return (int16_t)((code & 0x80) ? -magnitude : magnitude);
}

size_t audio_capture(uint8_t *packet, size_t samples) {
    int16_t pcm[AUDIO_PACKET_SAMPLES * HW_CHANNELS];
    if (!capture) return 0;
    if (samples > AUDIO_PACKET_SAMPLES) samples = AUDIO_PACKET_SAMPLES;
    snd_pcm_sframes_t n = snd_pcm_readi(capture, pcm, samples);
    if (n == -EPIPE && snd_pcm_prepare(capture) >= 0) snd_pcm_start(capture);
    if (n <= 0) return 0;
    for (snd_pcm_sframes_t i = 0; i < n; i++)
        packet[i] = pcm_to_mulaw((pcm[i * 2] + pcm[i * 2 + 1]) / 2);
    return (size_t)n;
}

size_t audio_play(const uint8_t *packet, size_t samples) {
    int16_t pcm[AUDIO_PACKET_SAMPLES * HW_CHANNELS];
    if (!playback) return 0;
    if (samples > AUDIO_PACKET_SAMPLES) samples = AUDIO_PACKET_SAMPLES;
    for (size_t i = 0; i < samples; i++)
        pcm[i * 2] = pcm[i * 2 + 1] = mulaw_to_pcm(packet[i]);
    snd_pcm_sframes_t n = snd_pcm_writei(playback, pcm, samples);
    if (n == -EPIPE && snd_pcm_prepare(playback) >= 0) {
        playback_pending = 0;
        n = snd_pcm_writei(playback, pcm, samples);
    }
    if (n <= 0) return 0;
    if (snd_pcm_state(playback) == SNDRV_PCM_STATE_PREPARED)
        playback_pending += (size_t)n;
    else
        playback_pending = 0;
    return (size_t)n;
}

void audio_start_pending(void) {
    if (playback_pending && snd_pcm_start(playback) >= 0)
        playback_pending = 0;
}

void audio_stop_playback(void) {
    if (!playback) return;
    playback_pending = 0;
    snd_pcm_drop(playback);
    snd_pcm_prepare(playback);
}

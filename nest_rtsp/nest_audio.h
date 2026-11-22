#ifndef NEST_AUDIO_H
#define NEST_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NEST_AUDIO_STATUS "/tmp/nest_audio.capabilities"
#define AUDIO_CAPTURE 1
#define AUDIO_PLAYBACK 2
#define AUDIO_RTP_RATE 8000
#define AUDIO_PACKET_SAMPLES 160

int audio_open(int requested);
void audio_close(void);
size_t audio_capture(uint8_t *packet, size_t samples);
size_t audio_play(const uint8_t *packet, size_t samples);
void audio_start_pending(void);
void audio_stop_playback(void);

#endif

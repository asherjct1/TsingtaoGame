#ifndef TSINGTAO_AUDIO_H
#define TSINGTAO_AUDIO_H

#include <stdint.h>

/*
 * Initialize SPU (master + CD-DA mixer volumes), upload all ADPCM SFX blobs
 * from g_sfx_table into SPU RAM starting at 0x1010, and probe CD-ROM TOC
 * for Red Book CD-DA Track 02 without blocking if no audio track exists.
 */
void audio_init(void);

/*
 * Trigger an SPU ADPCM sound effect on a round-robin voice (0..15).
 *   sfx_id      : enum SfxId value from assets/gen/sfx_table.h
 *   pan_x       : screen X position 0..320 (0=left, 160=center, 320=right)
 *   pitch_delta : signed pitch offset added to sample's native SPU pitch (0 = normal)
 */
void audio_play_sfx(int sfx_id, int pan_x, int pitch_delta);

/* Silence all active SFX voices */
void audio_stop_all_sfx(void);

/* Red Book CD-DA Track 02 BGM controls (safe no-ops if no Track 02 present) */
void audio_bgm_start(void);
void audio_bgm_stop(void);
void audio_bgm_pause(int paused);
void audio_bgm_set_muted(int muted);
int  audio_bgm_is_muted(void);
int  audio_bgm_is_playing(void);
int  audio_bgm_is_available(void);

/* Poll CD-DA playhead once per VSync (50 Hz) to guarantee seamless looping */
void audio_bgm_update(void);

#endif /* TSINGTAO_AUDIO_H */

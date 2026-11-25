#include "audio.h"
#include "../assets/gen/sfx_table.h"

#include <stddef.h>
#include <stdint.h>
#include <psxspu.h>
#include <psxcd.h>
#include <hwregs_c.h>

#define SPU_ALLOC_BASE       0x1010u
#define SPU_RAM_LIMIT        0x7FF00u
#define SPU_SFX_VOICE_COUNT  16
#define SPU_VOL_MASTER       0x3FFF
#define SPU_VOL_CD           0x3FFF
#define SPU_VOL_VOICE_MAX    0x3FFF
#define SPU_VOL_VOICE_MIN    0x1000

/* Uploaded SPU RAM byte addresses for each SFX */
static uint32_t s_sfx_spu_addr[SFX_COUNT];
static int      s_sfx_loaded[SFX_COUNT];
static int      s_next_voice = 0;

/* Red Book CD-DA Track 02 state */
static int             s_bgm_available    = 0;
static int             s_bgm_playing      = 0;
static int             s_bgm_paused       = 0;
static int             s_bgm_muted        = 0;
static volatile int    s_bgm_need_restart = 0;
static CdlLOC          s_track2_start;
static CdlLOC          s_track2_pause_pos;
static int             s_has_pause_pos    = 0;
static int             s_has_end_lba      = 0;
static int32_t         s_track2_end_lba   = 0;
static int             s_bgm_grace_frames = 0;
static int             s_bgm_poll_counter = 0;

static inline void spu_key_off(uint32_t voice_mask) {
    SPU_KEY_OFF1 = (uint16_t)(voice_mask & 0xFFFFu);
    SPU_KEY_OFF2 = (uint16_t)((voice_mask >> 16) & 0x00FFu);
}

static inline void spu_key_on(uint32_t voice_mask) {
    SPU_KEY_ON1 = (uint16_t)(voice_mask & 0xFFFFu);
    SPU_KEY_ON2 = (uint16_t)((voice_mask >> 16) & 0x00FFu);
}

static void on_cd_autopause(CdlIntrResult irq, uint8_t *result) {
    (void)result;
    if (irq == CdlDataEnd && s_bgm_playing && !s_bgm_paused) {
        s_bgm_need_restart = 1;
    }
}

static void apply_cd_volume(void) {
    if (s_bgm_muted || s_bgm_paused || !s_bgm_playing) {
        SpuSetCommonCDVolume(0, 0);
    } else {
        SpuSetCommonCDVolume(SPU_VOL_CD, SPU_VOL_CD);
    }
}

static void start_track2_internal(void) {
    uint8_t mode = (uint8_t)(CdlModeDA | CdlModeAP);

    s_bgm_need_restart = 0;
    s_has_pause_pos    = 0;
    s_bgm_grace_frames = 100; /* 2.0s at 50 Hz PAL for seek/spin-up */
    s_bgm_poll_counter = 0;

    CdControl(CdlSetmode, &mode, NULL);
    CdControl(CdlSetloc, &s_track2_start, NULL);
    CdControl(CdlPlay, NULL, NULL);

    apply_cd_volume();
}

void audio_init(void) {
    uint32_t spu_addr = SPU_ALLOC_BASE;
    int i;

    /* 1. Initialize PS1 SPU hardware and mixer volumes */
    SpuInit();

    /* Ensure SPU + DAC + CD-DA audio mixing bits are active, reverb disabled */
    SPU_REVERB_ON1 = 0;
    SPU_REVERB_ON2 = 0;
    SpuSetCommonCDReverb(0);
    SPU_CTRL |= 0x0001; /* Enable CD audio input into SPU mixer */
    SpuSetCommonMasterVolume(SPU_VOL_MASTER, SPU_VOL_MASTER);
    SpuSetCommonCDVolume(0, 0); /* Keep CD quiet until audio_bgm_start() */

    /* 2. Upload all 12 ADPCM SFX blobs into SPU RAM via 64B-aligned DMA */
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    for (i = 0; i < SFX_COUNT; i++) {
        uint32_t sz = g_sfx_table[i].size;
        uint32_t aligned_sz = (sz + 63u) & ~63u;

        s_sfx_loaded[i]   = 0;
        s_sfx_spu_addr[i] = 0;

        if (!g_sfx_table[i].data || sz == 0u) {
            continue;
        }
        if (spu_addr + aligned_sz > SPU_RAM_LIMIT) {
            continue;
        }

        SpuSetTransferStartAddr(spu_addr);
        SpuWrite((const uint32_t *)g_sfx_table[i].data, sz);
        SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

        s_sfx_spu_addr[i] = spu_addr;
        s_sfx_loaded[i]   = 1;
        spu_addr         += aligned_sz;
    }

    s_next_voice = 0;

    /* 3. Probe CD-ROM TOC for Red Book CD-DA Track 02 (safe if no disc/track) */
    s_bgm_available    = 0;
    s_bgm_playing      = 0;
    s_bgm_paused       = 0;
    s_bgm_muted        = 0;
    s_bgm_need_restart = 0;
    s_has_pause_pos    = 0;
    s_has_end_lba      = 0;
    s_track2_end_lba   = 0;

    if (CdInit()) {
        static const CdlATV atv = { 128, 0, 128, 0 };
        CdlLOC toc[99];
        int num_tracks;

        CdMix(&atv);
        num_tracks = CdGetToc(toc);
        if (num_tracks >= 2) {
            s_track2_start = toc[1]; /* 0-indexed: toc[0]=Track 1, toc[1]=Track 2 */

            /* Determine end of Track 2 for emulator-safe fallback loop detection */
            if (num_tracks >= 3) {
                s_track2_end_lba = CdPosToInt(&toc[2]);
                s_has_end_lba    = 1;
            } else {
                uint8_t zero = 0;
                uint8_t td_res[4];
                if (CdCommand(CdlGetTD, &zero, 1, td_res) &&
                    CdSync(1, NULL) == CdlComplete) {
                    CdlLOC leadout;
                    leadout.minute = td_res[1];
                    leadout.second = td_res[2];
                    leadout.sector = 0;
                    leadout.track  = 0;
                    s_track2_end_lba = CdPosToInt(&leadout);
                    if (s_track2_end_lba > CdPosToInt(&s_track2_start) + 150) {
                        s_has_end_lba = 1;
                    }
                }
            }

            CdAutoPauseCallback(on_cd_autopause);
            s_bgm_available = 1;
        }
    }
}

void audio_play_sfx(int sfx_id, int pan_x, int pitch_delta) {
    int ch;
    int32_t right_amt;
    uint16_t vol_l, vol_r;
    int32_t pitch;

    if (sfx_id < 0 || sfx_id >= SFX_COUNT || !s_sfx_loaded[sfx_id]) {
        return;
    }

    /* Round-robin across SPU voices 0..15 */
    ch = s_next_voice;
    s_next_voice = (s_next_voice + 1) % SPU_SFX_VOICE_COUNT;

    /* Clamp screen X to 0..320 and compute stereo pan volumes (pure integer) */
    if (pan_x < 0)   pan_x = 0;
    if (pan_x > 320) pan_x = 320;

    right_amt = ((int32_t)pan_x * (SPU_VOL_VOICE_MAX - SPU_VOL_VOICE_MIN)) / 320;
    vol_r     = (uint16_t)(SPU_VOL_VOICE_MIN + right_amt);
    vol_l     = (uint16_t)(SPU_VOL_VOICE_MAX - right_amt);

    /* Apply pitch variation with hardware-safe clamping */
    pitch = (int32_t)g_sfx_table[sfx_id].pitch + (int32_t)pitch_delta;
    if (pitch < 256)    pitch = 256;
    if (pitch > 0x3FFF) pitch = 0x3FFF;

    /*
     * Write voice registers and trigger KON directly without an immediately
     * preceding KOFF on the same voice: on real PS1 SPU silicon, KOFF and KON
     * are latched every 44.1 kHz sample (~768 CPU cycles), so back-to-back
     * KOFF+KON can cause KOFF to release/silence the voice on the same tick.
     */
    SPU_CH_VOL_L(ch) = vol_l;
    SPU_CH_VOL_R(ch) = vol_r;
    SPU_CH_FREQ(ch)  = (uint16_t)pitch;
    SPU_CH_ADDR(ch)  = getSPUAddr(s_sfx_spu_addr[sfx_id]);
    SPU_CH_ADSR1(ch) = 0x00FF;
    SPU_CH_ADSR2(ch) = 0x0000;
    spu_key_on(1u << ch);
}

void audio_stop_all_sfx(void) {
    spu_key_off(0x0000FFFFu);
}

void audio_bgm_start(void) {
    if (!s_bgm_available) {
        return;
    }
    s_bgm_playing = 1;
    s_bgm_paused  = 0;
    start_track2_internal();
}

void audio_bgm_stop(void) {
    if (!s_bgm_available) {
        return;
    }
    s_bgm_playing      = 0;
    s_bgm_paused       = 0;
    s_bgm_need_restart = 0;
    s_has_pause_pos    = 0;
    apply_cd_volume();
    CdControl(CdlPause, NULL, NULL);
}

void audio_bgm_pause(int paused) {
    if (!s_bgm_available || !s_bgm_playing) {
        return;
    }

    if (paused && !s_bgm_paused) {
        CdlLOCINFOP locp;
        s_bgm_paused = 1;
        apply_cd_volume();

        if (CdControl(CdlGetlocP, NULL, (uint8_t *)&locp) && locp.track == 0x02) {
            s_track2_pause_pos.minute = locp.minute;
            s_track2_pause_pos.second = locp.second;
            s_track2_pause_pos.sector = locp.sector;
            s_track2_pause_pos.track  = locp.track;
            s_has_pause_pos           = 1;
        } else {
            s_has_pause_pos = 0;
        }
        CdControl(CdlPause, NULL, NULL);
    } else if (!paused && s_bgm_paused) {
        uint8_t mode = (uint8_t)(CdlModeDA | CdlModeAP);
        s_bgm_paused       = 0;
        s_bgm_need_restart = 0;
        s_bgm_grace_frames = 50;

        CdControl(CdlSetmode, &mode, NULL);
        if (s_has_pause_pos) {
            CdControl(CdlSetloc, &s_track2_pause_pos, NULL);
        } else {
            CdControl(CdlSetloc, &s_track2_start, NULL);
        }
        CdControl(CdlPlay, NULL, NULL);
        apply_cd_volume();
    }
}

void audio_bgm_set_muted(int muted) {
    s_bgm_muted = muted ? 1 : 0;
    if (s_bgm_available) {
        apply_cd_volume();
    }
}

int audio_bgm_is_muted(void) {
    return s_bgm_muted;
}

int audio_bgm_is_playing(void) {
    return s_bgm_available && s_bgm_playing && !s_bgm_paused;
}

int audio_bgm_is_available(void) {
    return s_bgm_available;
}

void audio_bgm_update(void) {
    uint8_t stat = 0;

    if (!s_bgm_available || !s_bgm_playing || s_bgm_paused) {
        return;
    }

    /* Fast path: CD-ROM hardware INT4 CdlDataEnd auto-pause callback fired */
    if (s_bgm_need_restart) {
        start_track2_internal();
        return;
    }

    if (s_bgm_grace_frames > 0) {
        s_bgm_grace_frames--;
        return;
    }

    /* Safety poll once per second (50 PAL frames) for emulator/drive resilience */
    s_bgm_poll_counter++;
    if (s_bgm_poll_counter < 50) {
        return;
    }
    s_bgm_poll_counter = 0;

    if (CdControl(CdlNop, NULL, &stat)) {
        if (!(stat & (CdlStatPlay | CdlStatSeek))) {
            start_track2_internal();
            return;
        }
    }

    if (s_has_end_lba) {
        CdlLOCINFOP locp;
        if (CdControl(CdlGetlocP, NULL, (uint8_t *)&locp)) {
            CdlLOC cur;
            cur.minute = locp.minute;
            cur.second = locp.second;
            cur.sector = locp.sector;
            cur.track  = locp.track;

            if (locp.track != 0x02 || CdPosToInt(&cur) >= (s_track2_end_lba - 75)) {
                start_track2_internal();
            }
        }
    }
}

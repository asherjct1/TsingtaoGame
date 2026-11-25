#include "memcard.h"
#include "../assets/gen/mc_icon.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include <psxgpu.h>
#include <sys/fcntl.h>

/*
 * Real PAL PlayStation 1 Memory Card Save Format:
 *   Device/Filename : bu00:BESLES-09600TSING (BE = Europe PAL, SLES-09600 = SYSTEM.CNF)
 *   Size            : 1 block = 8192 bytes (64 x 128-byte sectors)
 *   0x000..0x001    : 'S', 'C'
 *   0x002           : 0x11 (1 static icon frame)
 *   0x003           : 0x01 (1 block)
 *   0x004..0x043    : Shift-JIS title ("ＴＳＩＮＧＴＡＯ　ＥＸＰＲＥＳＳ　９６")
 *   0x060..0x07F    : 16-color RGB555 CLUT (32 bytes)
 *   0x080..0x0FF    : 16x16 4-bit Tsingtao Can icon (128 bytes)
 *   0x200..0x1FFF   : Checksummed Top 5 High Score payload
 */
#define MC_SAVE_PATH       "bu00:BESLES-09600TSING"
#define MC_BLOCK_SIZE      8192
#define MC_READ_SIZE       1024 /* Multiple of 128 bytes; covers header + payload at 0x200 */
#define MC_PAYLOAD_OFFSET  0x200
#define MC_SAVE_MAGIC      0x54534736u /* "TSG6" */
#define MC_MAX_BLOCKS      15

typedef struct {
    uint32_t       magic;
    uint16_t       version;
    uint16_t       count;
    HighScoreEntry scores[MC_MAX_SCORES];
    uint32_t       checksum;
} McSavePayload;

_Static_assert(sizeof(HighScoreEntry) == 12, "HighScoreEntry must be 12 bytes");
_Static_assert(sizeof(McSavePayload) == 72, "McSavePayload v1/v2 binary compatibility check");

/* Aligned static 8 KB staging buffer (keeps stack usage tiny) */
static uint8_t s_mc_block[MC_BLOCK_SIZE] __attribute__((aligned(4)));

static int s_mc_inited = 0;

static uint32_t calc_payload_checksum(const McSavePayload *p) {
    /* FNV-1a over all bytes before the checksum field */
    const uint8_t *bytes = (const uint8_t *)p;
    size_t len = offsetof(McSavePayload, checksum);
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= (uint32_t)bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static void write_sjis_title(uint8_t *dst, size_t max_bytes, const char *ascii) {
    size_t out = 0;

    memset(dst, 0, max_bytes);
    while (*ascii && (out + 2u) <= max_bytes) {
        char c = *ascii++;
        if (c >= 'A' && c <= 'Z') {
            dst[out++] = 0x82u;
            dst[out++] = (uint8_t)(0x60u + (uint8_t)(c - 'A'));
        } else if (c >= '0' && c <= '9') {
            dst[out++] = 0x82u;
            dst[out++] = (uint8_t)(0x4Fu + (uint8_t)(c - '0'));
        } else if (c == ' ') {
            dst[out++] = 0x81u;
            dst[out++] = 0x40u;
        } else if (c == '\'') {
            dst[out++] = 0x81u;
            dst[out++] = 0x66u;
        } else {
            dst[out++] = 0x81u;
            dst[out++] = 0x40u;
        }
    }
}

/*
 * Scoped SwCARD event wait (peak 4 BIOS EvCB slots, 0 leaked) for
 * _card_info() and _card_load().
 * Returns: 0=IOE, 1=NEW, 2=TIMOUT, 3=ERROR
 */
static int run_sw_card_op(int (*op)(int), int chan, int max_vsyncs) {
    int ev_ioe, ev_err, ev_tmo, ev_new;
    int exit_cs;
    int res = 2;
    int i;

    exit_cs = EnterCriticalSection();
    ev_ioe = OpenEvent(SwCARD, EvSpIOE,    EvMdNOINTR, NULL);
    ev_err = OpenEvent(SwCARD, EvSpERROR,  EvMdNOINTR, NULL);
    ev_tmo = OpenEvent(SwCARD, EvSpTIMOUT, EvMdNOINTR, NULL);
    ev_new = OpenEvent(SwCARD, EvSpNEW,    EvMdNOINTR, NULL);
    if (ev_ioe >= 0) EnableEvent(ev_ioe);
    if (ev_err >= 0) EnableEvent(ev_err);
    if (ev_tmo >= 0) EnableEvent(ev_tmo);
    if (ev_new >= 0) EnableEvent(ev_new);
    if (exit_cs) ExitCriticalSection();

    if (ev_ioe >= 0) TestEvent(ev_ioe);
    if (ev_err >= 0) TestEvent(ev_err);
    if (ev_tmo >= 0) TestEvent(ev_tmo);
    if (ev_new >= 0) TestEvent(ev_new);

    op(chan);

    for (i = 0; i < max_vsyncs; i++) {
        if (ev_ioe >= 0 && TestEvent(ev_ioe)) { res = 0; break; }
        if (ev_new >= 0 && TestEvent(ev_new)) { res = 1; break; }
        if (ev_tmo >= 0 && TestEvent(ev_tmo)) { res = 2; break; }
        if (ev_err >= 0 && TestEvent(ev_err)) { res = 3; break; }
        VSync(0);
    }

    exit_cs = EnterCriticalSection();
    if (ev_ioe >= 0) { DisableEvent(ev_ioe); CloseEvent(ev_ioe); }
    if (ev_err >= 0) { DisableEvent(ev_err); CloseEvent(ev_err); }
    if (ev_tmo >= 0) { DisableEvent(ev_tmo); CloseEvent(ev_tmo); }
    if (ev_new >= 0) { DisableEvent(ev_new); CloseEvent(ev_new); }
    if (exit_cs) ExitCriticalSection();

    return res;
}

/*
 * Scoped HwCARD event wait (peak 3 BIOS EvCB slots, 0 leaked) for
 * _card_clear(), which signals HwCARD rather than SwCARD on real PS1 BIOS.
 * Returns: 0=IOE, 2=TIMOUT, 3=ERROR
 */
static int run_hw_card_op(int (*op)(int), int chan, int max_vsyncs) {
    int ev_ioe, ev_err, ev_tmo;
    int exit_cs;
    int res = 2;
    int i;

    exit_cs = EnterCriticalSection();
    ev_ioe = OpenEvent(HwCARD, EvSpIOE,    EvMdNOINTR, NULL);
    ev_err = OpenEvent(HwCARD, EvSpERROR,  EvMdNOINTR, NULL);
    ev_tmo = OpenEvent(HwCARD, EvSpTIMOUT, EvMdNOINTR, NULL);
    if (ev_ioe >= 0) EnableEvent(ev_ioe);
    if (ev_err >= 0) EnableEvent(ev_err);
    if (ev_tmo >= 0) EnableEvent(ev_tmo);
    if (exit_cs) ExitCriticalSection();

    if (ev_ioe >= 0) TestEvent(ev_ioe);
    if (ev_err >= 0) TestEvent(ev_err);
    if (ev_tmo >= 0) TestEvent(ev_tmo);

    op(chan);

    for (i = 0; i < max_vsyncs; i++) {
        if (ev_ioe >= 0 && TestEvent(ev_ioe)) { res = 0; break; }
        if (ev_tmo >= 0 && TestEvent(ev_tmo)) { res = 2; break; }
        if (ev_err >= 0 && TestEvent(ev_err)) { res = 3; break; }
        VSync(0);
    }

    exit_cs = EnterCriticalSection();
    if (ev_ioe >= 0) { DisableEvent(ev_ioe); CloseEvent(ev_ioe); }
    if (ev_err >= 0) { DisableEvent(ev_err); CloseEvent(ev_err); }
    if (ev_tmo >= 0) { DisableEvent(ev_tmo); CloseEvent(ev_tmo); }
    if (exit_cs) ExitCriticalSection();

    return res;
}

static MemCardStatus probe_and_mount_card(void) {
    int ev;

    if (!s_mc_inited) {
        return MC_ERROR;
    }

    ev = run_sw_card_op(_card_info, 0, 60);
    if (ev == 0) return MC_OK;
    if (ev == 2) return MC_NO_CARD;
    if (ev == 3) return MC_ERROR;

    /* ev == 1 (EvSpNEW): newly inserted or swapped card — clear (HwCARD) then load (SwCARD) */
    ev = run_hw_card_op(_card_clear, 0, 40);
    if (ev == 2) return MC_NO_CARD;
    if (ev == 3) return MC_ERROR;

    ev = run_sw_card_op(_card_load, 0, 90);
    if (ev == 0) return MC_OK;
    if (ev == 1) return MC_UNFORMATTED;
    if (ev == 2) return MC_NO_CARD;
    return MC_ERROR;
}

static int count_used_blocks(void) {
    struct DIRENTRY de;
    struct DIRENTRY *p;
    int blocks = 0;

    p = firstfile("bu00:*", &de);
    while (p != NULL) {
        int b = (p->size + (MC_BLOCK_SIZE - 1)) / MC_BLOCK_SIZE;
        if (b < 1) b = 1;
        blocks += b;
        p = nextfile(&de);
    }
    return blocks;
}

void memcard_init(void) {
    if (s_mc_inited) {
        return;
    }

    /*
     * InitCARD(1) keeps controller polling active alongside memory card I/O
     * on real PS1 hardware. Call after input_init().
     */
    InitCARD(1);
    StartCARD();
    _bu_init();

    s_mc_inited = 1;
}

void memcard_load_defaults(HighScoreEntry scores[MC_MAX_SCORES]) {
    static const HighScoreEntry k_defaults[MC_MAX_SCORES] = {
        { "TSG", 120000u, 8u, 2u, 1u },
        { "QDO",  65000u, 6u, 2u, 0u },
        { "B96",  32000u, 4u, 1u, 0u },
        { "PSX",  14000u, 2u, 2u, 0u },
        { "CAN",   4500u, 1u, 2u, 0u }
    };
    memcpy(scores, k_defaults, sizeof(k_defaults));
}

MemCardStatus memcard_load_scores(HighScoreEntry scores[MC_MAX_SCORES]) {
    MemCardStatus st;
    int fd;
    int nread;
    McSavePayload payload;
    int i;

    /* Always populate clean 1996 Qingdao defaults first */
    memcard_load_defaults(scores);

    st = probe_and_mount_card();
    if (st != MC_OK) {
        return st;
    }

    fd = open(MC_SAVE_PATH, FREAD);
    if (fd < 0) {
        return MC_NO_FILE;
    }

    memset(s_mc_block, 0, MC_READ_SIZE);
    nread = read(fd, s_mc_block, MC_READ_SIZE);
    close(fd);

    if (nread != MC_READ_SIZE) {
        return MC_ERROR;
    }
    if (s_mc_block[0] != 'S' || s_mc_block[1] != 'C') {
        return MC_ERROR;
    }

    memcpy(&payload, &s_mc_block[MC_PAYLOAD_OFFSET], sizeof(payload));
    if (payload.magic != MC_SAVE_MAGIC ||
        (payload.version != 1u && payload.version != MC_SAVE_VERSION) ||
        payload.count != MC_MAX_SCORES ||
        payload.checksum != calc_payload_checksum(&payload)) {
        return MC_ERROR;
    }

    for (i = 0; i < MC_MAX_SCORES; i++) {
        scores[i] = payload.scores[i];
        scores[i].initials[3] = '\0';
        if (payload.version == 1u) {
            /* Upgrade v1 HighScoreEntry (where bytes 10..11 were pad=0) cleanly to v2 */
            scores[i].shift = 0u;
            scores[i].won   = 0u;
        } else {
            if (scores[i].shift > 2u) {
                scores[i].shift = 2u;
            }
            scores[i].won = scores[i].won ? 1u : 0u;
        }
    }
    return MC_OK;
}

MemCardStatus memcard_save_scores(const HighScoreEntry scores[MC_MAX_SCORES]) {
    MemCardStatus st;
    McSavePayload payload;
    int fd;
    int nwritten;
    int i;

    st = probe_and_mount_card();
    if (st != MC_OK) {
        return st;
    }

    /* Assemble authentic 1-block 8192-byte PlayStation 1 save frame */
    memset(s_mc_block, 0, MC_BLOCK_SIZE);
    s_mc_block[0] = 'S';
    s_mc_block[1] = 'C';
    s_mc_block[2] = 0x11u; /* 1-frame static icon */
    s_mc_block[3] = 0x01u; /* 1 block (8 KB) */

    /* Shift-JIS title: ＴＳＩＮＧＴＡＯ　ＥＸＰＲＥＳＳ　９６ */
    write_sjis_title(&s_mc_block[0x004], 64u, "TSINGTAO EXPRESS 96");

    /* 16-color RGB555 CLUT (32 bytes at 0x060) & 16x16 4-bit Tsingtao Can icon (128 bytes at 0x080) */
    memcpy(&s_mc_block[0x060], MC_ICON_CLUT, MC_ICON_CLUT_BYTES);
    memcpy(&s_mc_block[0x080], MC_ICON_BITMAP, MC_ICON_DATA_BYTES);

    /* Checksummed Top 5 High Scores payload at 0x200 */
    memset(&payload, 0, sizeof(payload));
    payload.magic   = MC_SAVE_MAGIC;
    payload.version = MC_SAVE_VERSION;
    payload.count   = MC_MAX_SCORES;
    for (i = 0; i < MC_MAX_SCORES; i++) {
        payload.scores[i] = scores[i];
        payload.scores[i].initials[3] = '\0';
        if (payload.scores[i].shift > 2u) {
            payload.scores[i].shift = 2u;
        }
        payload.scores[i].won = payload.scores[i].won ? 1u : 0u;
    }
    payload.checksum = calc_payload_checksum(&payload);
    memcpy(&s_mc_block[MC_PAYLOAD_OFFSET], &payload, sizeof(payload));

    /* Overwrite existing save in place if present, else create a new 1-block file */
    fd = open(MC_SAVE_PATH, FWRITE);
    if (fd < 0) {
        fd = open(MC_SAVE_PATH, FWRITE | FCREATE | FNBLOCKS(1));
        if (fd < 0) {
            if (count_used_blocks() >= MC_MAX_BLOCKS) {
                return MC_FULL;
            }
            return MC_ERROR;
        }
    }

    nwritten = write(fd, s_mc_block, MC_BLOCK_SIZE);
    close(fd);

    return (nwritten == MC_BLOCK_SIZE) ? MC_OK : MC_ERROR;
}

int memcard_qualifies(const HighScoreEntry scores[MC_MAX_SCORES], uint32_t score) {
    if (score == 0u) {
        return 0;
    }
    return score > scores[MC_MAX_SCORES - 1].score;
}

int memcard_insert_score_ex(HighScoreEntry scores[MC_MAX_SCORES],
                            const char initials[4], uint32_t score,
                            uint16_t night, uint8_t shift, uint8_t won) {
    int rank = -1;
    int i;

    if (score == 0u) {
        return -1;
    }

    for (i = 0; i < MC_MAX_SCORES; i++) {
        if (score > scores[i].score) {
            rank = i;
            break;
        }
    }
    if (rank < 0) {
        return -1;
    }

    for (i = MC_MAX_SCORES - 1; i > rank; i--) {
        scores[i] = scores[i - 1];
    }

    scores[rank].initials[0] = initials[0] ? initials[0] : 'A';
    scores[rank].initials[1] = initials[1] ? initials[1] : 'A';
    scores[rank].initials[2] = initials[2] ? initials[2] : 'A';
    scores[rank].initials[3] = '\0';
    scores[rank].score       = score;
    scores[rank].wave        = night;
    scores[rank].shift       = (shift <= 2u) ? shift : 2u;
    scores[rank].won         = won ? 1u : 0u;

    return rank;
}

int memcard_insert_score(HighScoreEntry scores[MC_MAX_SCORES],
                         const char initials[4], uint32_t score, uint16_t wave) {
    return memcard_insert_score_ex(scores, initials, score, wave, 0u, 0u);
}

const char *memcard_status_str(MemCardStatus st) {
    switch (st) {
        case MC_OK:          return "SAVED TO MEMORY CARD!";
        case MC_NO_CARD:     return "NO MEMORY CARD - SESSION SAVED";
        case MC_UNFORMATTED: return "CARD UNFORMATTED - SESSION SAVED";
        case MC_FULL:        return "MEMORY CARD FULL - SESSION SAVED";
        case MC_NO_FILE:     return "NO SAVE FILE - DEFAULTS LOADED";
        default:             return "CARD ERROR - SESSION SAVED";
    }
}

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SAMPLE: IMA ADPCM sample sets. */
static const char *const N_ONOFF_E[] = {"OFF", "ON"};
/* IMA ADPCM sample sets (tools/gen_samples.py), native rates, zones across
 * the keyboard, loops with the ADPCM state stored at the loop start.
 * SET is P_E0 (EDIT 1, KNOB 1). SET 4 was PERC, the GM drum kit: retired, an alias of PIANO; its sounds
 * load as the DRUM engine (core.h drum_from_perc). */
typedef struct {
    uint32_t off, n, ls, le;     /* byte offset, samples, loop start / end (sample index) */
    uint32_t rate;               /* source rate / 44100, Q16 */
    int16_t root16;              /* root pitch in 1/16 semitone */
    int16_t pred;                /* ADPCM state at the loop start */
    uint8_t idx, lo, hi, looped;
} smp_zone_t;
typedef struct {
    const char *name;
    uint16_t z0, nz;
} smp_set_t;
#define SMP_DATA SMP_DATA_BUILTIN                       /* (the built-ins' bytes under their own name: */
#include "felucca_samples.h"                            /*  without them, the compiler leaves them out) */
#undef SMP_DATA
#if FELUCCA_BUILTIN
#define SMP_DATA SMP_DATA_BUILTIN
#else
static const uint8_t SMP_DATA[4];                       /* no built-in sample: the user slots' base, as before */
#endif
#if defined(SMP_PERC_SLOT) && SMP_PERC_SLOT != SMP_SET_PERC
#error "tools/gen_samples.py PERC_SLOT != core.h SMP_SET_PERC"
#endif

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
/* 2^(i/192), Q16: pitch ratios in 1/16 semitones, d16 >= -3072 (16 octaves down) */
static uint32_t pow2_q16(int32_t d16)
{
    uint32_t u = (uint32_t)(d16 + 192 * 16), oct = u / 192u;     /* no loop for negative d16 */
    uint32_t r = pitch_inc(1600 + u % 192u) / (pitch_inc(1600) >> 16);   /* 2^(d/192) via the pitch table */
    return oct >= 16u ? r << (oct - 16u) : r >> (16u - oct);
}

/* ---- user sample slots (loaded from the web editor into flash, see web/EDITOR_PROTOCOL.md)
 * 3 slots of 80 KiB at flash 0xA0000.. (Felucca data region), read through the plain XIP
 * window. The mangler (FELUCCA_MANGLER) adds USR4 at 0xE7000..0xFAFFF: FM6's voice-bank sector (no FM6 here) and
 * the spare flash above it that fm1_flash.h keeps for a firmware's own data (FL_XDATA1); its sectors' bytes 0xF00..
 * must never read as an update record (smp_rec_guard). Slot = header (magic, count, name, data length, CRC32) + up to 16 zones in the
 * smp_zone_t layout (off relative to the slot's data at +512) + IMA ADPCM data. */
#include "../bsp/chip/fm1_xip.h"   /* relative: hostsim includes this file too */
#if FELUCCA_MANGLER
#define SMP_USER_SLOTS 4
#else
#define SMP_USER_SLOTS 3
#endif
#define SMP_USER_BASE 0xA0000u
#define SMP_USER4_BASE 0xE7000u                     /* USR4 (the mangler) */
#define SMP_USER_SIZE 0x14000u
#define SMP_USER_DATA 512u
#define SMP_USER_MAGIC 0x504D5346u                  /* "FSMP" */
#define SMP_NALL (SMP_NSETS + SMP_USER_SLOTS)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t nz, rsv;
    char name[8];
    uint32_t data_len, crc, rsv2[2];
    smp_zone_t zone[16];
} smp_user_hdr_t;                                   /* 32 + 16 x 28 = 480 B, data at +512 */
static smp_zone_t usr_zone[SMP_USER_SLOTS][16];     /* RAM copy, off rebased onto SMP_DATA */
/* SET / SRC v as played: without the built-ins (FELUCCA_BUILTIN 0) one of theirs is USR1 */
static inline uint32_t smp_si(int32_t v)
{
    uint32_t si = (uint32_t)v % SMP_NALL;
    return FELUCCA_BUILTIN || si >= SMP_NSETS ? si : SMP_NSETS;
}
static uint8_t usr_nz[SMP_USER_SLOTS];
#if FELUCCA_MANGLER
static const char *const SMP_ALL_NAMES[SMP_NALL] = {SMP_SET_NAMES_INIT, "USR1", "USR2", "USR3", "USR4"};
#else
static const char *const SMP_ALL_NAMES[SMP_NALL] = {SMP_SET_NAMES_INIT, "USR1", "USR2", "USR3"};
#endif
/* slot k's flash offset */
static inline uint32_t smp_user_off(uint32_t k) { return k < 3u ? SMP_USER_BASE + k * SMP_USER_SIZE : SMP_USER4_BASE; }
_Static_assert(SMP_USER_SLOTS < 4 || SMP_USER4_BASE + SMP_USER_SIZE <= 0xFC000u, "USR4 below Felucca's globals");
/* bytes about to be programmed at flash offset off: the update loader (and so, presumably, the SPL) takes a 4 KiB
 * sector's bytes 0xF00.. for an update record when 6..7 there read "AT" and 0..1 hold a CRC-16 (0x1021, from 0) of the 78
 * after them, not 0; the loader then erases the sector (ldr_core.c). Sample data that happens to read so (about once in
 * 2^32 sectors) gets one bit of byte 0xF06 flipped: a step of one ADPCM code. A write that covers 0xF00..0xF4F whole is
 * checked (the editor's 256-byte writes and the bounce's sectors do); USR4 only: the loader skips Felucca's store */
static uint32_t smp_crc16(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0, i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++)
            c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
        c &= 0xFFFFu;
    }
    return c;
}
static void smp_rec_guard(uint32_t off, uint8_t *buf, uint32_t n)
{
    uint32_t a = (off & ~0xFFFu) + 0xF00u;
    for (; a + 80u <= off + n; a += 0x1000u)
        if (a >= off) {
            uint8_t *r = buf + (a - off);
            uint32_t c = (uint32_t)r[0] | (uint32_t)r[1] << 8;
            if (r[6] == 0x41u && r[7] == 0x54u && c && c == smp_crc16(r + 2, 78))
                r[6] ^= 1u;
        }
}

#ifndef SMP_USER_XIP                                /* host tests: a RAM image of the slots */
#define SMP_USER_XIP(k) fm1_xip_ptr(smp_user_off(k))
#endif
static const uint8_t *smp_user_xip(uint32_t k) { return SMP_USER_XIP(k); }
/* user slot k's sample name as given when it was loaded (upper case, trailing spaces off; b holds 9): 0 = empty slot
 * or no name */
static int usr_name(uint32_t k, char *b)
{
    const smp_user_hdr_t *h;
    uint32_t i, n = 0;
    b[0] = 0;
    if (k >= SMP_USER_SLOTS || !usr_nz[k])
        return 0;
    h = (const smp_user_hdr_t *)smp_user_xip(k);
    for (i = 0; i < 8u && h->name[i]; i++) {
        char c = h->name[i];
        b[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c >= ' ' && c <= '~' ? c : ' ';
        if (b[i] != ' ')
            n = i + 1u;
    }
    b[n] = 0;
    return n > 0;
}
#if FELUCCA_SLICE
static void slc_user_scan(uint32_t k, int valid);   /* eng_slice.c: SLICE's slice table of the slot */
#else
#define slc_user_scan(k, valid) ((void)0)
#endif
static uint32_t smp_user_gen;                       /* + 1 per slot scan (GRAIN: its seek index follows uploads) */

/* (re)read slot k from flash: valid header -> zones usable; call after boot and after an upload
 * (main loop: SLICE scans the slot's audio here). The zones are checked in a copy: an unusable slot leaves
 * every zone of it empty (n 0: a voice still on it ends), never a half-checked one */
static void smp_user_scan(uint32_t k)
{
    const smp_user_hdr_t *h = (const smp_user_hdr_t *)smp_user_xip(k);
    smp_zone_t zt[16];
    uint32_t i, base, nz = 0;
    smp_user_gen++;
    usr_nz[k] = 0;
    slc_user_scan(k, 0);
    if (h->magic == SMP_USER_MAGIC && h->version == 1 && h->nz && h->nz <= 16u &&
        h->data_len <= SMP_USER_SIZE - SMP_USER_DATA) {
        base = (uint32_t)(uintptr_t)(smp_user_xip(k) + SMP_USER_DATA) - (uint32_t)(uintptr_t)SMP_DATA;
        for (nz = h->nz, i = 0; i < nz; i++) {
            smp_zone_t *z = &zt[i];
            *z = h->zone[i];
            if (z->off > h->data_len || z->n > 2u * SMP_USER_SIZE || z->off + (z->n + 1u) / 2u > h->data_len ||
                z->idx > 88u || !z->rate || z->rate > 4u << 16 || z->ls > z->le || z->le >= z->n || z->lo > z->hi) {
                nz = 0;                             /* zone outside the data or malformed: slot unusable */
                break;
            }
            z->off += base;
        }
    }
    for (i = 0; i < 16u; i++)
        usr_zone[k][i] = i < nz ? zt[i] : (smp_zone_t){0};
    if (!nz)
        return;
    usr_nz[k] = (uint8_t)nz;
    slc_user_scan(k, 1);
}

/* A missing sample (SAMPLE, GRAIN, SLICE: an empty or invalid user slot, a set this build has no data for) plays
 * this instead, like a hardware sampler with its sample gone: a plain sine at the note's pitch (+ d16, 1/16
 * semitones), at half the peak of a full sample, through the engine's one-pole low-pass (lp Q15, state *y) and the
 * voice's amp (the ADSR: no click at the start, phase 0, or the end). *ph its phase. The UI says so once
 * (engines.c snd_missing, ui_input.c sample_notice). */
static void smp_sine(int32_t *out, uint32_t n, const vmod_t *m, int32_t d16, int32_t lp, uint32_t *ph, int32_t *y)
{
    uint32_t i, p = *ph, inc = pitch_inc((uint32_t)clamp(m->pitch16 + d16, 0, 2047));
    int32_t s = *y;
    for (i = 0; i < n; i++, p += inc) {
        s += mulq15((sine_i(p) >> 1) - s, lp);
        out[i] += voice_amp(s, m, i) << 1;
    }
    *ph = p;
    *y = s;
}
/* SET / SRC si (0..SMP_NALL - 1) has no sample data: an empty or invalid user slot, a built-in set without data */
static int smp_set_missing(uint32_t si)
{
    if (si < SMP_NSETS)
        return !SMP_ZONES[SMP_SETS[si].z0].n;
    return !usr_nz[(si - SMP_NSETS) % SMP_USER_SLOTS];
}

/* zone index in a voice: < 0x8000 built-in, else 0x8000 | slot << 5 | zone */
static inline const smp_zone_t *smp_zone(uint32_t zi)
{
    return zi < 0x8000u ? &SMP_ZONES[zi] : &usr_zone[(zi >> 5) & 3u][zi & 31u];
}

/* voice: ph[0] position (samples), ph[1] fraction Q16, s[0] predictor, s[1] step
 * index, s[2] previous sample, s[3] current sample, s[4] zone, s[5] step Q16 */
static inline int32_t sample_next(const smp_zone_t *z, voice_t *v, int loop)
{
    uint32_t pos = v->ph[0], b = SMP_DATA[z->off + (pos >> 1)];
    uint32_t code = (pos & 1u) ? (b >> 4) : (b & 15u);
    int32_t step = IMA_STEP[(uint32_t)v->s[1] <= 88u ? v->s[1] : 88], vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    v->s[0] = clamp(v->s[0] + ((code & 8u) ? -vd : vd), -32768, 32767);
    v->s[1] = clamp(v->s[1] + IMA_IDX[code & 7u], 0, 88);
    pos++;
    if (pos > z->le && z->looped && loop) {
        pos = z->ls;
        v->s[0] = z->pred;
        v->s[1] = z->idx;
    }
    v->ph[0] = pos;
    return v->s[0];
}

/* ---- SHARD (2026-11-15): START, LEN and PLAY. A voice reads its zone through a two-segment cache (256 samples each,
 * per voice, in the part's engine memory: engines.c eng_mem) decoded from a seek index (the ADPCM state before every
 * 256th sample of each zone of the SET, as GRAIN's: built a few entries a block by sample_block, and by the voices
 * themselves as they decode forward). Any position in either direction is then plain arithmetic: a loop of START..END,
 * a ping-pong, a reverse, START moved while a loop plays (a wavetable scan), at one decode per source sample as before.
 * PLAY (P_E3, was LOOP OFF / ON): ONE one-shot, SMPL the zone's own loop (as LOOP ON), LOOP START..END, PONG back and
 * forth, REV a reverse one-shot, RLOOP a reverse loop. START (P_E5) 0..127 of the zone; LEN (P_E7) the length from
 * START on the ENV time scale, 127 (10 s) the rest of the zone; a stored 0 (a project or preset of before: the slot
 * was unused) is the rest of the zone too (LEN's minimum is 1). */
/* 4-point interpolation (SMP_CR4, the mangler: SAMPLE and SHARD; 2026-11-19): Catmull-Rom between x0 and x1 at frac
 * (Q16), its neighbours xm1 and x2. 10 kHz in a 22050 Hz sample comes out -4.4 dB (linear: -6.3), and the editor's
 * upload filter (web/editor.html resampleHq, tools/sampleio.py resample_hq) makes up the rest: flat to 9.9 kHz.
 * The phase in Q11 keeps every product in 32 bits for any int16 samples. */
#ifndef SMP_CR4
#define SMP_CR4 FELUCCA_MANGLER
#endif
static inline int32_t smp_cr4(int32_t xm1, int32_t x0, int32_t x1, int32_t x2, uint32_t frac)
{
    int32_t f = (int32_t)(frac >> 5), a = 3 * (x0 - x1) + x2 - xm1, b = 2 * xm1 - 5 * x0 + 4 * x1 - x2, c = x1 - xm1;
    return x0 + (((((((a * f) >> 11) + b) * f) >> 11) + c) * f >> 12);
}
enum { SX_ONE, SX_SMPL, SX_LOOP, SX_PONG, SX_REV, SX_RLOOP };
static const char *const SX_PLAY_NAMES[] = {"ONE", "SMPL", "LOOP", "PONG", "REV", "RLOOP"};
#define SX_SEG_LOG2 8
#define SX_SEG (1u << SX_SEG_LOG2)
#define SX_NIDX 1024                /* index entries per part: a full user slot (80 KB, 160k samples) is 640 */
#define SX_MAXZ 32
typedef struct {
    int16_t buf[NVOICE][2][SX_SEG];   /* each voice's two cached segments */
    int16_t ipred[SX_NIDX];         /* the index: the decoder state before sample k * SX_SEG of a zone */
    uint8_t iidx[SX_NIDX];
    uint16_t zbase[SX_MAXZ], zcnt[SX_MAXZ], zdone[SX_MAXZ];   /* a zone's entries: first, all, built */
    uint32_t stamp;                 /* the user slots' generation it was built for */
    uint16_t gen;                   /* bumped on each rebuild: the voices' caches of before are stale */
    uint8_t src;                    /* SET + 1 the index holds, 0 = none */
    uint8_t nz;
    uint8_t want;                   /* a voice waits for an entry: build faster */
#if SMP_CR4
    int16_t hx[NVOICE][2];          /* each voice's x[-1] and x[2] around s[2], s[3] (smp_cr4: one sample later) */
    uint8_t tail[NVOICE];           /* zeros fed past a one-shot's end */
#endif
} sx_part_t;
ENG_MEM_FITS("SAMPLE", sizeof(sx_part_t));
static sx_part_t *sx_of(const track_t *t) { return (sx_part_t *)ENG_MEM((uint32_t)(t - trk) % NPART); }
static uint32_t sx_nz(uint32_t si) { return si < SMP_NSETS ? SMP_SETS[si].nz : usr_nz[(si - SMP_NSETS) % SMP_USER_SLOTS]; }
static const smp_zone_t *sx_zone(uint32_t si, uint32_t zl)
{
    return si < SMP_NSETS ? &SMP_ZONES[SMP_SETS[si].z0 + zl] : &usr_zone[(si - SMP_NSETS) % SMP_USER_SLOTS][zl & 15u];
}
/* a voice's zone (s[4]) as the set's zone number, SX_MAXZ = none */
static uint32_t sx_zl(uint32_t si, uint32_t zi)
{
    if (zi & 0x8000u)
        return si >= SMP_NSETS ? zi & 31u : SX_MAXZ;
    return si < SMP_NSETS && zi >= SMP_SETS[si].z0 && zi - SMP_SETS[si].z0 < SMP_SETS[si].nz ? zi - SMP_SETS[si].z0 : SX_MAXZ;
}
static inline int32_t sx_dec(const smp_zone_t *z, uint32_t pos, int32_t *pred, int32_t *idx)
{
    uint32_t b = SMP_DATA[z->off + (pos >> 1)], code = (pos & 1u) ? (b >> 4) : (b & 15u);
    int32_t step = IMA_STEP[*idx], vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    *pred = clamp(*pred + ((code & 8u) ? -vd : vd), -32768, 32767);
    *idx = clamp(*idx + IMA_IDX[code & 7u], 0, 88);
    return *pred;
}
/* the index for SET si (its stamp: built-in 0, a user slot its generation) */
static void sx_check(sx_part_t *P, uint32_t si)
{
    uint32_t i, tot = 0, nz, stamp = si < SMP_NSETS ? 0u : smp_user_gen;
    if (P->src == si + 1u && P->stamp == stamp)
        return;
    nz = sx_nz(si);
    P->src = (uint8_t)(si + 1u);
    P->stamp = stamp;
    P->gen++;
    P->nz = (uint8_t)(nz < SX_MAXZ ? nz : SX_MAXZ);
    for (i = 0; i < P->nz; i++) {
        uint32_t c = (sx_zone(si, i)->n + SX_SEG - 1u) >> SX_SEG_LOG2;
        if (tot + c > SX_NIDX)
            c = 0;                                  /* past the capacity: that zone stays silent */
        P->zbase[i] = (uint16_t)tot;
        P->zcnt[i] = (uint16_t)c;
        P->zdone[i] = c ? 1u : 0u;
        if (c) {
            P->ipred[tot] = 0;                      /* a zone starts at the state (0, 0) */
            P->iidx[tot] = 0;
        }
        tot += c;
    }
}
/* one more entry: zone zl's (SX_MAXZ: the first not done) */
static void sx_build(sx_part_t *P, uint32_t zl)
{
    uint32_t i, k, e, pos;
    int32_t pred, idx;
    const smp_zone_t *z;
    if (zl >= P->nz || P->zdone[zl] >= P->zcnt[zl])
        for (zl = 0; zl < P->nz && P->zdone[zl] >= P->zcnt[zl]; zl++)
            ;
    if (zl >= P->nz)
        return;
    z = sx_zone(P->src - 1u, zl);
    k = P->zdone[zl];
    e = P->zbase[zl] + k - 1u;
    pred = P->ipred[e];
    idx = P->iidx[e];
    pos = (k - 1u) << SX_SEG_LOG2;
    for (i = 0; i < SX_SEG && pos + i < z->n; i++)
        sx_dec(z, pos + i, &pred, &idx);
    P->ipred[e + 1u] = (int16_t)pred;
    P->iidx[e + 1u] = (uint8_t)idx;
    P->zdone[zl] = (uint16_t)(k + 1u);
}
/* sample p of the voice's zone through its cache: ph[2] the two segment tags + 1 (bits 0..15, 16..31); s[5] bit 11 the
 * way used last, bits 16..24 the index generation the tags belong to (a rebuilt index: nothing cached is valid); bits
 * 8..10 the voice's buffers. (The fields keep clear of masks in 0xFFC0_0000..0xFFCF_FFFF: build.py's ROM check.)
 * -32769 = its segment is not indexed yet (the voice waits) */
#define SX_WAIT (-32769)
static int32_t sx_at(sx_part_t *P, voice_t *v, const smp_zone_t *z, uint32_t zl, uint32_t p)
{
    uint32_t seg = p >> SX_SEG_LOG2, tag = seg + 1u, c = v->ph[2], f = (uint32_t)v->s[5], w, i, e;
    uint32_t gen = (uint32_t)P->gen & 511u, vb = (f >> 8) & 7u;
    int16_t *b;
    int32_t pred, idx;
    if (((f >> 16) & 511u) != gen) {
        c = 0;
        f = (f & 0xFFFFu) | gen << 16;
    }
    if ((c & 0xFFFFu) == tag)
        w = 0;
    else if ((c >> 16) == tag)
        w = 1;
    else {
        if (zl >= P->nz || seg >= P->zdone[zl]) {
            v->ph[2] = c;
            v->s[5] = (int32_t)f;
            P->want = 1;
            return SX_WAIT;
        }
        w = (f >> 11) & 1u ? 0u : 1u;               /* the way not used last */
        b = P->buf[vb][w];
        e = P->zbase[zl] + seg;
        pred = P->ipred[e];
        idx = P->iidx[e];
        for (i = 0; i < SX_SEG && (seg << SX_SEG_LOG2) + i < z->n; i++)
            b[i] = (int16_t)sx_dec(z, (seg << SX_SEG_LOG2) + i, &pred, &idx);
        if (seg + 1u == P->zdone[zl] && seg + 1u < P->zcnt[zl]) {   /* (it builds the index as it goes) */
            P->ipred[e + 1u] = (int16_t)pred;
            P->iidx[e + 1u] = (uint8_t)idx;
            P->zdone[zl] = (uint16_t)(seg + 2u);
        }
        c = w ? (c & 0xFFFFu) | tag << 16 : (c >> 16) << 16 | tag;
    }
    v->ph[2] = c;
    v->s[5] = (int32_t)(w ? f | 1u << 11 : f & ~(1u << 11));
    return P->buf[vb][w][p & (SX_SEG - 1u)];
}
/* START..END of zone z for the track's values (END exclusive, >= START + 2) */
static void sx_region(const int16_t *p, const smp_zone_t *z, uint32_t *lo, uint32_t *hi)
{
    uint32_t n = z->n, a = muldiv32((uint32_t)clamp(p[P_E5], 0, 127), n, 128u), len, hz;
    int32_t lv = p[P_E7];
    if (n < 2u) {
        *lo = 0;
        *hi = n;
        return;
    }
    if (a > n - 2u)
        a = n - 2u;
    if (lv <= 0 || lv >= 127)
        len = n;                                    /* the rest of the zone */
    else {
        hz = (uint32_t)(((uint64_t)z->rate * 44100u) >> 16);   /* the zone's rate, Hz */
        len = muldiv32(TIME_MS_X10[lv], hz, 10000u);
    }
    if (len < 2u)
        len = 2u;
    *lo = a;
    *hi = len >= n - a ? n : a + len;
}
/* the next source sample in PLAY's order; the position ph[0] (the index to read), s[5] bit 0 the direction (1 back),
 * bits 8..10 the voice's buffers (sx_at: bit 11, 16..24 its cache's); SX_WAIT, or s[6] = 1 when a one-shot is past its end */
static int32_t sx_next(sx_part_t *P, voice_t *v, const smp_zone_t *z, uint32_t zl, const int16_t *p)
{
    uint32_t lo, hi, pos = v->ph[0], mode = (uint32_t)clamp(p[P_E3], 0, SX_RLOOP);
    int32_t back = v->s[5] & 1, x;
    sx_region(p, z, &lo, &hi);
    if (mode == SX_SMPL && z->looped)
        hi = z->n;                                  /* (the zone's loop: LEN does not end it) */
    if (pos >= z->n)
        pos = z->n - 1u;
    x = sx_at(P, v, z, zl, pos);
    if (x == SX_WAIT)
        return x;
    switch (mode) {
    case SX_ONE: case SX_SMPL: default:
        pos++;
        if (mode == SX_SMPL && z->looped && pos > z->le) {
            pos = z->ls;
            x = z->pred;                            /* (as LOOP ON did: the last sample before a wrap is the one
                                                     * before the loop start, its stored state: bit for bit) */
        }
        else if (pos >= hi)
            pos = z->n;                             /* (past the end: the one-shot ends, sample_render) */
        break;
    case SX_LOOP:
        pos = pos + 1u >= hi || pos + 1u < lo ? lo : pos + 1u;
        break;
    case SX_PONG:
        if (!back) {
            if (pos + 1u >= hi) { back = 1; pos = hi >= 2u ? hi - 2u : 0u; } else pos++;
        } else {
            if (pos <= lo || pos > hi) { back = 0; pos = lo + 1u < hi ? lo + 1u : lo; } else pos--;
        }
        break;
    case SX_REV:
        if (pos <= lo || pos >= hi) pos = z->n; else pos--;
        break;
    case SX_RLOOP:
        pos = pos <= lo || pos >= hi ? hi - 1u : pos - 1u;
        break;
    }
    v->s[5] = (v->s[5] & ~1) | back;
    v->ph[0] = pos;
    return x;
}
/* the first position for PLAY (the note's start): REV and RLOOP from the end, the rest from START */
static void sx_start(const track_t *t, voice_t *v)
{
    const smp_zone_t *z = smp_zone((uint32_t)v->s[4]);
    uint32_t lo, hi, mode = (uint32_t)clamp(t->p[P_E3], 0, SX_RLOOP);
    if (!z->n)
        return;
    sx_region(t->p, z, &lo, &hi);
    v->ph[0] = mode == SX_REV || mode == SX_RLOOP ? hi - 1u : lo;
}

static void sample_note_on(track_t *t, voice_t *v)
{
    uint32_t si = smp_si(t->p[P_E0]), i, zi = 0xFFFFu, width = 128u;
    if (si < SMP_NSETS) {                           /* a built-in set: its zones split the keyboard */
        const smp_set_t *set = &SMP_SETS[si];
        for (i = 0; i < set->nz; i++)
            if (v->note >= SMP_ZONES[set->z0 + i].lo && v->note <= SMP_ZONES[set->z0 + i].hi) {
                zi = set->z0 + i;
                break;
            }
        v->s[4] = (int32_t)(zi == 0xFFFFu ? set->z0 : zi);
    } else {                                        /* user slot: silent if empty; the narrowest zone plays */
        uint32_t k = si - SMP_NSETS;
        for (i = 0; i < usr_nz[k]; i++) {
            const smp_zone_t *z = &usr_zone[k][i];
            if (v->note >= z->lo && v->note <= z->hi && z->hi - z->lo <= width) {
                zi = 0x8000u | k << 5 | i;
                width = z->hi - z->lo;
            }
        }
        v->s[4] = (int32_t)(zi == 0xFFFFu ? 0 : zi);
    }
    v->ph[0] = 0;
    v->ph[1] = 0;
    v->ph[2] = 0;                /* the segment cache: empty */
    v->s[0] = 0;
    v->s[1] = 0;
    v->s[2] = v->s[3] = 0;
    v->s[5] = (int32_t)((uint32_t)(v - t->v) % NVOICE) << 8;   /* forward; its buffers */
#if SMP_CR4
    {
        sx_part_t *P = sx_of(t);
        uint32_t vi = (uint32_t)(v - t->v) % NVOICE;
        P->hx[vi][0] = P->hx[vi][1] = 0;
        P->tail[vi] = 0;
    }
#endif
    v->s[6] = zi == 0xFFFFu;     /* 1 = sample ended (one-shot); a kit key with no sound stays silent */
    v->s[7] = 0;                 /* lo-pass state */
    if (smp_set_missing(si))
        v->s[6] = 2;             /* 2 = no sample data: the sine (smp_sine; ph[0] its phase) */
    else if (!v->s[6])
        sx_start(t, v);
}

/* each block: the index for the SET, built on (faster while a voice waits) */
static void sample_block(track_t *t)
{
    sx_part_t *P = sx_of(t);
    uint32_t si = smp_si(t->p[P_E0]), k, nb;
    if (smp_set_missing(si))
        return;
    sx_check(P, si);
    nb = P->want ? 4u : 1u;
    P->want = 0;
    for (k = 0; k < nb; k++)
        sx_build(P, SX_MAXZ);
}

static void sample_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const smp_zone_t *z = smp_zone((uint32_t)v->s[4]);
    uint32_t i, frac = v->ph[1], si = smp_si(p[P_E0]), zl;
    int32_t d16 = clamp(m->pitch16 + p[P_E1] * 16 - z->root16, -1536, 576);   /* <= 3 octaves up: bounded decode load */
    uint32_t r = pow2_q16(d16), stepq = (r >> 8) * (z->rate >> 8);   /* Q16 samples per output */
    int32_t bits = p[P_E2], lp = 4000 + ((clamp((p[P_E4] << 8) + m->cutoff, 0, 127 << 8) * 28767) >> 15);
    int32_t drv = p[P_E6], sh = bits / 10;
    sx_part_t *P = sx_of(t);
    if (v->s[6] == 2) {                               /* no sample data: the sine at the note's pitch + TUNE */
        smp_sine(out, n, m, p[P_E1] * 16, lp, &v->ph[0], &v->s[7]);
        return;
    }
    if (v->s[6] || !z->n) {
        v->active = 0;
        return;
    }
    sx_check(P, si);
    zl = sx_zl(si, (uint32_t)v->s[4]);
#if SMP_CR4
    {   /* (one source sample later than the linear one: s[2] .. s[3] between hx[0] and hx[1]) */
        uint32_t vi = (uint32_t)(v - t->v) % NVOICE;
        int16_t *h = P->hx[vi];
        if (v->ph[0] < z->n && v->ph[1] == 0 && v->s[3] == 0 && v->s[2] == 0 && !(v->s[5] & 2)) {
            int32_t x = sx_next(P, v, z, zl, p);      /* prime the interpolator */
            if (x == SX_WAIT)
                return;                               /* (its segment not indexed yet: a block later) */
            h[1] = (int16_t)x;
            v->s[5] |= 2;
        }
        for (i = 0; i < n; i++) {
            int32_t s;
            frac += stepq;
            while (frac >= 65536u) {
                int32_t x = 0;
                if (v->ph[0] >= z->n) {               /* a one-shot's end: zeros in, then it stops at the last */
                    if (++P->tail[vi] >= 3u) {
                        v->s[6] = 1;
                        frac &= 65535u;
                        break;
                    }
                } else {
                    x = sx_next(P, v, z, zl, p);
                    if (x == SX_WAIT) {               /* (a segment not indexed yet: hold this sample) */
                        frac = 65535u;
                        break;
                    }
                }
                frac -= 65536u;
                h[0] = (int16_t)v->s[2];
                v->s[2] = v->s[3];
                v->s[3] = h[1];
                h[1] = (int16_t)x;
            }
            s = v->s[6] ? 0 : smp_cr4(h[0], v->s[2], v->s[3], h[1], frac);
#else
    {
    if (v->ph[0] < z->n && v->ph[1] == 0 && v->s[3] == 0 && v->s[2] == 0 && !(v->s[5] & 2)) {
        int32_t x = sx_next(P, v, z, zl, p);          /* prime the interpolator */
        if (x == SX_WAIT)
            return;                                   /* (its segment not indexed yet: a block later) */
        v->s[3] = x;
        v->s[5] |= 2;
    }
    for (i = 0; i < n; i++) {
        int32_t s;
        frac += stepq;
        while (frac >= 65536u) {
            int32_t x;
            if (v->ph[0] >= z->n) {                   /* one-shot reached its end */
                v->s[2] = v->s[3];
                v->s[6] = 1;
                v->s[3] = 0;
                frac -= 65536u;
                if (frac >= 65536u) {                /* past the last sample-to-zero interval */
                    v->s[2] = 0;
                    frac &= 65535u;
                }
                break;
            }
            x = sx_next(P, v, z, zl, p);
            if (x == SX_WAIT) {                       /* (a segment not indexed yet: hold this sample) */
                frac = 65535u;
                break;
            }
            frac -= 65536u;
            v->s[2] = v->s[3];
            v->s[3] = x;
        }
        s = v->s[2] + (((v->s[3] - v->s[2]) * (int32_t)(frac >> 1)) >> 15);
#endif
        if (sh)                                       /* BITS: 0 = clean, up to 12 bits removed */
            s = (s >> sh) << sh;
        if (drv)
            s = softclip(s + (((s >> 2) * (drv * 150)) >> 11));   /* = s * drv * 600 / 32768, no overflow */
        v->s[7] += mulq15(s - v->s[7], lp);           /* gentle tone control (CUT) */
        out[i] += voice_amp(v->s[7], m, i) << 1;
        if (v->s[6])
            break;
    }
    }
    v->ph[1] = frac;
}

#if !FELUCCA_BUILTIN
static const preset_t SMP_USR_PRESETS[] = {
    {"USR1", {SMP_NSETS, 0, 0, 1, 127, 0, 0, 127}, {0, 127, 127, 70}, 0, 0},
    {"USR2", {SMP_NSETS + 1, 0, 0, 1, 127, 0, 0, 127}, {0, 127, 127, 70}, 0, 0},
    {"USR3", {SMP_NSETS + 2, 0, 0, 1, 127, 0, 0, 127}, {0, 127, 127, 70}, 0, 0},
    {"USR4", {SMP_NSETS + 3, 0, 0, 1, 127, 0, 0, 127}, {0, 127, 127, 70}, 0, 0},
};
_Static_assert(SMP_USER_SLOTS == 4, "SAMPLE's presets: one a user slot");
#endif
static const engine_t ENG_SAMPLE = {
    .name = "SAMPLE",
    .page_title = {"SET", "TRIM"},
    .edit = {
        {"SET", F_ENUM, 0, SMP_NALL - 1, FELUCCA_BUILTIN ? 0 : SMP_NSETS, SMP_ALL_NAMES, 0},
        {"TUNE", F_SEMI, -24, 24, 0, 0, 0},
        {"BITS", F_INT, 0, 127, 0, 0, 0},
        {"PLAY", F_ENUM, 0, SX_RLOOP, SX_SMPL, SX_PLAY_NAMES, 0},   /* (SHARD; was LOOP OFF / ON = ONE / SMPL) */
        {"CUT", F_INT, 0, 127, 127, 0, 0},
        {"START", F_PCT, 0, 127, 0, 0, 0},                          /* (SHARD) */
        {"DRV", F_PCT, 0, 127, 0, 0, 0},
        {"LEN", F_TIME, 1, 127, 127, 0, 0},                         /* (SHARD) 127: the rest of the zone */
    },
#if FELUCCA_BUILTIN
    .presets = SMP_PRESET_TABLE,
    .npresets = SMP_NPRESETS,
#else
    .presets = SMP_USR_PRESETS,                     /* (the user slots, as PIANO's preset plays a set) */
    .npresets = SMP_USER_SLOTS,
#endif
    .note_on = sample_note_on,
    .render = sample_render,
    .knob = {P_E0, P_E4, P_ATK, P_REL},
    .sampled = 1,
    .block = sample_block,
    .mem = sizeof(sx_part_t),
};

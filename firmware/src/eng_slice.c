/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SLICE: a sample slicer, played from the keys and the sequencer. Felucca's own design.
 *
 * Material (SRC): the built-in BREAK (tools/gen_samples.py: one bar of 16ths at 120 BPM arranged
 * from Felucca's generated drums, stored after the SAMPLE sets), a user slot USR1..3 (the same
 * slots as SAMPLE's, eng_sample.c), whose zones are played one after the other as one recording
 * (a zone that shares its data with an earlier one is skipped), or PIANO (SRC 4, since 1.0.4: one
 * note, the SAMPLE engine's PIANO zone of middle C, played from its data in SMP_DATA: no copy).
 * A source with no material (an empty or invalid slot; PIANO in a build without the CC0 samples)
 * plays a plain sine at the key's pitch (+ PTCH) instead, through DCAY, TONE and the ADSR (MODE ONE
 * acts as GATE there: no slice end), and the UI says so (ui_input.c sample_notice).
 *
 * Slices (DIV): 4 / 8 / 16 / 32 equal ones (SHARD also 2 / 6 / 12), AUTO: one per onset, or MAN: set by hand on the EDIT family's SLICES
 * page (ui_slice.c) for a user slot (BREAK and PIANO: MAN plays AUTO). IMA ADPCM can only be entered
 * at a known decoder state, so each source keeps a table (slc_src_t): the states at the 192 grid
 * points k * len / 192 (every equal slice starts on one, and they are the reverse checkpoints) and
 * up to 32 AUTO starts with their states. BREAK's and PIANO's tables are written at build time (BREAK's
 * hits are its AUTO slices; PIANO has one, the attack); a user slot's is computed by slc_scan when the slot becomes valid
 * (smp_user_scan: boot, upload end; main loop, never the audio ISR): two decoding passes; per hop
 * of 32 samples the peak of the first difference (hats, snares, clicks stand out, a kick's tail
 * does not) and the plain peak (low hits); an onset is a hop where either rises 1.75x over its
 * follower (~23 ms / ~93 ms, so a low tone's cycles are no rises), above 1/12 (1/8) of its
 * largest value, at least 50 ms after the last one; the slice starts one hop before the hop that
 * rose (1.5..3 ms of pre-roll, never after the hit). RAM: 3 x 972 B of tables, 3 KiB of reverse
 * windows; a full 7 s slot scans in ~0.3 M decoder steps.
 *
 * Playing: note n plays slice (n - 60 - ROOT + START) mod the slice count (the keys: the lowest
 * key is slice 0, seq.c kb_map). Slices play at the source rate times PITCH (varispeed: faster is
 * higher). MODE: ONE plays the slice to its end (a note-off does not stop it), GATE until the
 * note-off (then the ADSR release) or the slice end, LOOP repeats the slice while the note is held.
 * REV plays a slice backwards: windows of 64 samples are decoded forwards from the nearest
 * checkpoint (grid point or the slice start) into the voice's own buffer and read from the top
 * down; it costs about (len / 192) / 128 extra decodes per source sample (BREAK: ~3; a full
 * 7 s user slot: ~10). DECAY: an exponential fade per slice (127 = none). TONE: a gentle low-pass
 * (SAMPLE's CUT). The last ~1.5 ms of a slice fade out (no click at the cut), a new slice fades in
 * over one block (0.7 ms).
 * Voice state: ph[0] position (reverse: position + 1), ph[1] fraction Q16, ph[2] slice end (reverse:
 * its start), s[0] predictor (reverse: window start), s[1] step index, s[2] / s[3] previous / current
 * sample, s[4] source | segment << 3 | reverse << 7 | slice << 8 | DIV << 16 (source SLC_SINE: the sine, its
 * phase in ph[0]), s[5] DECAY level Q15, s[6] 0 to prime, 1 playing, 3 fading out, 2 ended, s[7] low-pass. */
#define SLC_GRID 192u                /* grid points: decoder states at k * len / SLC_GRID (192 = 64 x 3: every equal
                                      * DIV, 2 4 6 8 12 16 32 slices, starts on one) */
#define SLC_AUTO 32u                 /* AUTO slices at most */
#define SLC_SEGS 16u                 /* zones of a user slot */
#define SLC_RB 64u                   /* reverse: samples decoded per window */
#define SLC_BASE 60                  /* note of slice 0 (+ the track's ROOT) */
#define SLC_HOP 32u                  /* AUTO detector: samples per hop */

typedef struct {
    uint32_t off, at, n;             /* data (SMP_DATA index), material position of its first sample, samples */
} slc_seg_t;
typedef struct {
    uint32_t len, rate, nseg, nauto; /* material samples (0 = none), rate (as smp_zone_t), segments, AUTO slices */
    slc_seg_t seg[SLC_SEGS];
    uint32_t grid[SLC_GRID];         /* decoder state before sample k * len / SLC_GRID: */
                                     /* (uint16_t)predictor | step index << 16 | segment << 24 */
    uint32_t apos[SLC_AUTO], ast[SLC_AUTO];   /* AUTO slice starts (apos[0] = 0) and their states */
} slc_src_t;

static const slc_src_t SLC_BREAK = SLC_BREAK_INIT;
static const slc_src_t SLC_PIANO = SLC_PIANO_INIT;
static slc_src_t slc_usr[SMP_USER_SLOTS];
typedef int16_t slc_rbuf_t[NVOICE][SLC_RB];           /* reverse windows, one per part voice (the part's eng_mem) */
ENG_MEM_FITS("SLICE", sizeof(slc_rbuf_t));
/* append-only: stored sounds keep their SRC numbers (1.0.4 added PIANO) */
#if FELUCCA_MANGLER
static const char *const N_SLC_SRC[] = {"BREAK", "USR1", "USR2", "USR3", "PIANO", "USR4"};   /* (USR4 after PIANO:
                                                                                 * the stored numbers stay) */
#define SLC_NSRC 6
#else
static const char *const N_SLC_SRC[] = {"BREAK", "USR1", "USR2", "USR3", "PIANO"};
#define SLC_NSRC 5
#endif
#define SLC_SRC_PIANO 4u
/* user slot k's SRC, and an SRC's user slot (SMP_USER_SLOTS: none, BREAK and PIANO) */
static inline uint32_t slc_src_of_slot(uint32_t k) { return k < 3u ? k + 1u : 5u; }
static inline uint32_t slc_slot_of_src(uint32_t src)
{
    return src >= 1u && src <= 3u ? src - 1u : src == 5u && SMP_USER_SLOTS > 3 ? 3u : (uint32_t)SMP_USER_SLOTS;
}
#define SLC_SINE 7u                  /* a voice's source when it has no material: the sine */
static const char *const N_SLC_DIV[] = {"4", "8", "16", "32", "AUTO", "MAN"};
static const char *const N_SLC_MODE[] = {"ONE", "GATE", "LOOP"};
static const char *const N_SLC_REV[] = {"OFF", "ON"};
enum { SLC_ONE, SLC_GATE, SLC_LOOP };
#define SLC_DIV_AUTO 4u
#define SLC_DIV_MAN 5u               /* the slice starts set by hand (the SLICES page); none yet: the AUTO ones */
/* (SHARD's DIV list adds 2, 6 and 12 equal slices: codes after SLICE's six, eng_shard.c SHD_DIV_CODE) */
#define SLC_DIV_2 6u
#define SLC_DIV_6 7u
#define SLC_DIV_12 8u
#define SLC_NDIV 9u
static const uint8_t SLC_DIV_EQ[SLC_NDIV] = {4, 8, 16, 32, 0, 0, 2, 6, 12};   /* equal slices of a code, 0: none */
#define SLC_MIN 64u                  /* MAN: the shortest slice (samples) */

/* MAN slices (based on hugelton/Felucca#27 by andreahaku): per user slot, in RAM; slice_store.c keeps them in flash
 * with the sample. Two copies: the audio side reads slc_man[k][slc_man_cur[k]], the SLICES page (main loop) changes
 * the other one and then flips slc_man_cur (one byte), so a note-on never sees a half-edited table. n = 0: none */
typedef struct {
    uint32_t n;                      /* slices, 0 = none */
    uint32_t end;                    /* where the last slice ends (0 = the material's end) */
    uint32_t pos[SLC_AUTO], st[SLC_AUTO];     /* slice starts and their decoder states (as ast) */
} slc_man_t;
static slc_man_t slc_man[SMP_USER_SLOTS][2];
static volatile uint8_t slc_man_cur[SMP_USER_SLOTS];
static void (*slc_man_load)(uint32_t k);        /* slice_store.c: a slot was read (smp_user_scan): its stored slices */
static uint8_t slc_man_save;                     /* bit k: slot k's slices changed, to flash (slice_store.c) */

/* the SRC of a part's sound (0..4: a stored value out of range reads as the nearest) */
static uint32_t slc_src_of(const int16_t *p) { return (uint32_t)clamp(p[P_E0], 0, SLC_NSRC - 1); }
/* source 0 = BREAK, 1..3 = USR1..3, 4 = PIANO, 5 = USR4 (the mangler); 0 = no material (an empty or erased slot, no
 * PIANO in the build) */
static const slc_src_t *slc_get(uint32_t src)
{
    if (!src)
        return SLC_BREAK.len ? &SLC_BREAK : 0;
    if (src == SLC_SRC_PIANO)
        return SLC_PIANO.len ? &SLC_PIANO : 0;
    src = slc_slot_of_src(src);
    return src < SMP_USER_SLOTS && usr_nz[src] && slc_usr[src].len ? &slc_usr[src] : 0;
}

/* ---- the ADPCM decoder over the material (segments one after the other, each from 0 / 0) */
typedef struct {
    int32_t pred, idx;
    uint32_t pos, seg;
} slc_dec_t;
static inline void slc_dec_at(slc_dec_t *d, uint32_t pos, uint32_t st)
{
    d->pos = pos;
    d->pred = (int16_t)(st & 0xFFFFu);
    d->idx = (int32_t)((st >> 16) & 127u);
    d->seg = st >> 24;
}
static inline uint32_t slc_dec_st(const slc_dec_t *d)
{
    return (uint32_t)(uint16_t)d->pred | (uint32_t)d->idx << 16 | d->seg << 24;
}
/* the sample at d->pos (< len), then the next position */
static inline int32_t slc_dec_next(const slc_src_t *s, slc_dec_t *d)
{
    const slc_seg_t *g = &s->seg[d->seg];
    uint32_t rel = d->pos - g->at, b, code;
    int32_t step, vd;
    if (rel >= g->n && d->seg + 1u < s->nseg) {         /* into the next zone: its data starts from 0 / 0 */
        d->seg++;
        g++;
        rel = d->pos - g->at;
        d->pred = 0;
        d->idx = 0;
    }
    b = SMP_DATA[g->off + (rel >> 1)];
    code = (rel & 1u) ? (b >> 4) : (b & 15u);
    step = IMA_STEP[d->idx];
    vd = step >> 3;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    d->pred = clamp(d->pred + ((code & 8u) ? -vd : vd), -32768, 32767);
    d->idx = clamp(d->idx + IMA_IDX[code & 7u], 0, 88);
    d->pos++;
    return d->pred;
}

static inline uint32_t slc_gpos(const slc_src_t *s, uint32_t k) { return k * s->len / SLC_GRID; }
/* the user slot of s (0..2), SMP_USER_SLOTS for BREAK / PIANO; its MAN slices in use, 0 = none (BREAK, PIANO: none) */
static uint32_t slc_slot_of(const slc_src_t *s)
{
    return s >= slc_usr && s < slc_usr + SMP_USER_SLOTS ? (uint32_t)(s - slc_usr) : SMP_USER_SLOTS;
}
static const slc_man_t *slc_man_of(const slc_src_t *s)
{
    uint32_t k = slc_slot_of(s);
    const slc_man_t *m;
    if (k >= SMP_USER_SLOTS)
        return 0;
    m = &slc_man[k][slc_man_cur[k] & 1u];
    return m->n ? m : 0;
}
static uint32_t slc_man_end(const slc_src_t *s, const slc_man_t *m) { return m->end && m->end < s->len ? m->end : s->len; }
static uint32_t slc_count(const slc_src_t *s, uint32_t div)
{
    const slc_man_t *m = div == SLC_DIV_MAN ? slc_man_of(s) : 0;
    return div < SLC_NDIV && SLC_DIV_EQ[div] ? SLC_DIV_EQ[div] : m ? m->n : s->nauto;
}
/* the slice a note plays (n slices): note - C4 - ROOT + START, mod n */
static uint32_t slc_note_slice(const int16_t *p, uint32_t note, uint32_t n)
{
    int32_t j = ((int32_t)note - SLC_BASE - p[P_ROOT] + p[P_E2]) % (int32_t)n;
    return (uint32_t)(j < 0 ? j + (int32_t)n : j);
}

/* slice j of DIV div: [*a, *b), *st the state at *a */
static void slc_bounds(const slc_src_t *s, uint32_t div, uint32_t j, uint32_t *a, uint32_t *b, uint32_t *st)
{
    const slc_man_t *m = div == SLC_DIV_MAN ? slc_man_of(s) : 0;
    if (div < SLC_NDIV && SLC_DIV_EQ[div]) {
        uint32_t w = SLC_GRID / SLC_DIV_EQ[div];
        *a = slc_gpos(s, j * w);
        *b = slc_gpos(s, (j + 1u) * w);
        *st = s->grid[j * w];
    } else if (m) {
        j = j < m->n ? j : m->n - 1u;
        *a = m->pos[j];
        *b = j + 1u < m->n ? m->pos[j + 1u] : slc_man_end(s, m);
        *st = m->st[j];
    } else {
        j = j < s->nauto ? j : s->nauto - 1u;
        *a = s->apos[j];
        *b = j + 1u < s->nauto ? s->apos[j + 1u] : s->len;
        *st = s->ast[j];
    }
}
static void slc_bounds_v(const slc_src_t *s, const voice_t *v, uint32_t *a, uint32_t *b, uint32_t *st)
{
    uint32_t pk = (uint32_t)v->s[4];
    slc_bounds(s, (pk >> 16) & 15u, (pk >> 8) & 63u, a, b, st);
}

/* ---- DETECT NOTES (slc_scan's pass 3, a user slot's): a slice where the pitch steps from one steady note to
 * another, surer where the level dips there too (the crossover: one note fading as the next comes in). The material
 * decimated to ~5.5 kHz (the mean of dec samples); every SN_FR of those a frame: the pitch (YIN: the cumulative-mean
 * normalised difference over SN_WIN, its first dip under 0.2 between 1.1 kHz and 50 Hz, the lag refined between its
 * neighbours; none: unpitched) and the level (the peak over the frame), both at the analysis window's middle. At a
 * frame i: SN_W frames before and after (but the one either side, mixed) all pitched and each steady (within 0.35 semitone), the two means a semitone
 * or more apart (a bend glides: not steady; noise: unpitched); the step's score min(1, semitones / 5); the dip the
 * lowest level within SN_W frames under the lower of the highs either side, as a fraction of it. A dip of 8 % or
 * more with the step: (step + dip) x 0.8 (the crossover), else step x 0.5; a slice from 0.8 - 0.6 x SENS / 127 on,
 * at the dip's bottom (else the step), moved on to the next rising zero crossing (12.5 ms at most: a low note's cycle). */
#define SN_FR 64u                           /* decimated samples per frame (~11.6 ms) */
#define SN_WIN 192u                         /* the pitch window (~35 ms) */
#define SN_LMAX 120u                        /* the longest lag (with fd ~5.5 kHz: 46 Hz) */
#define SN_RING 512u                        /* decimated samples kept (>= SN_WIN + SN_LMAX + SN_FR) */
#define SN_W 6u                             /* frames either side (~70 ms) */
#define SN_FQ 16u                           /* frames kept (>= 2 SN_W + 3) */
#define SN_MAX SLC_AUTO                     /* note slices at most */
enum { SLC_DET_HITS, SLC_DET_NOTES, SLC_DET_BOTH, SLC_NDET };
static const char *const N_SLC_DET[] = {"HITS", "NOTES", "BOTH"};
#define SLC_DET_DEF (FELUCCA_MANGLER ? SLC_DET_BOTH : SLC_DET_HITS)

/* log2(x) in Q12, x > 0 (as keytouch.c's: within 0.006 octave) */
static int32_t slc_lg12(uint32_t x)
{
    uint32_t n = 31u - (uint32_t)__builtin_clz(x), f;
    f = (n >= 16u ? x >> (n - 16u) : x << (16u - n)) & 0xFFFFu;
    f += (((f * (65536u - f)) >> 16) * 22487u) >> 16;
    return (int32_t)(n << 12) + (int32_t)(f >> 4);
}
/* the decoder state at pos of material len samples long (the grid as slc_scan left it: s->len is still 0) */
static void slc_dec_seek(const slc_src_t *s, uint32_t len, uint32_t pos, slc_dec_t *d)
{
    uint32_t k = muldiv32(pos, SLC_GRID, len);
    k = k < SLC_GRID ? k : SLC_GRID - 1u;
    while (k && muldiv32(k, len, SLC_GRID) > pos)
        k--;
    slc_dec_at(d, muldiv32(k, len, SLC_GRID), s->grid[k]);
    while (d->pos < pos)
        slc_dec_next(s, d);
}

static struct {
    int16_t x[SN_RING];                     /* the decimated material (>> 4: 12 bits) */
    int32_t p[SN_FQ];                       /* each frame's pitch (Q12 octaves; INT32_MIN: unpitched) */
    int32_t lv[SN_FQ];                      /* .. its level */
    uint32_t at[SN_FQ];                     /* .. its source sample */
    uint32_t pos[SN_MAX];                   /* the note slices found, */
    int32_t sc[SN_MAX];                     /* .. their scores */
    uint32_t n, near;                       /* source samples within which two are one (the higher score) */
} sn;

/* frame f's pitch from the ring's last SN_WIN + SN_LMAX samples (n written so far), lag range lo .. hi */
static int32_t sn_pitch(uint32_t n, uint32_t lo, uint32_t hi, uint32_t fd16)
{
    uint32_t b = n - SN_WIN - hi, t, j;
    uint32_t sum = 0;                       /* (32-bit: the device has no 64 / 64 division; each lag's dd <= 96 x 2^16) */
    uint32_t best = 0;
    uint32_t dn[SN_LMAX + 2];               /* the normalised difference, Q10 */
    dn[0] = 1024u;
    for (t = 1; t <= hi + 1u && t <= SN_LMAX + 1u; t++) {
        uint32_t dd = 0;
        for (j = 0; j < SN_WIN; j += 2u) {   /* (every 2nd: the dips as deep, half the work) */
            int32_t e = sn.x[(b + j) & (SN_RING - 1u)] - sn.x[(b + j + t) & (SN_RING - 1u)];
            dd += (uint32_t)(e * e) >> 8;
        }
        sum += dd;
        dn[t] = sum >= 1024u ? dd * t / (sum >> 10) : 1024u;   /* (dd t < 2^30: 32-bit) */
    }
    for (t = lo; t <= hi; t++)
        if (dn[t] < 205u) {                 /* the first dip under 0.2 .. */
            while (t < hi && dn[t + 1u] < dn[t])
                t++;                        /* .. to its bottom */
            best = t;
            break;
        }
    if (!best)
        return INT32_MIN;
    {
        int32_t a = (int32_t)dn[best - 1u], c = (int32_t)dn[best], e = (int32_t)dn[best + 1u], den = a - 2 * c + e;
        int32_t q16 = (int32_t)best * 16 + (den > 0 ? clamp((a - e) * 8 / den, -8, 8) : 0);   /* the lag, Q4 */
        return slc_lg12(fd16) - slc_lg12((uint32_t)(q16 > 16 ? q16 : 16));
    }
}

/* at frame c (the ring's: SN_W frames on): a note slice? */
static void sn_judge(uint32_t c, uint32_t sens)
{
    int32_t mb = 0, ma = 0, lob = INT32_MAX, hib = INT32_MIN, loa = INT32_MAX, hia = INT32_MIN, hl = 0, hr = 0, lo = INT32_MAX;
    uint32_t j, lowat = c, semis8, at;
    int32_t step, dip = 0, sc, thr;
    for (j = 2; j <= SN_W + 1u; j++) {          /* (the frames either side of c straddle the change: left out) */
        int32_t b = sn.p[(c - j) % SN_FQ], a = sn.p[(c + j) % SN_FQ];
        if (b == INT32_MIN || a == INT32_MIN)
            return;
        mb += b;
        ma += a;
        lob = b < lob ? b : lob;
        hib = b > hib ? b : hib;
        loa = a < loa ? a : loa;
        hia = a > hia ? a : hia;
    }
    if ((hib - lob) * 12 > 4096 * 35 / 100 || (hia - loa) * 12 > 4096 * 35 / 100)
        return;                             /* not steady (a bend, vibrato) */
    mb /= (int32_t)SN_W;
    ma /= (int32_t)SN_W;
    semis8 = (uint32_t)(ma > mb ? ma - mb : mb - ma) * 12u * 8u / 4096u;      /* semitones, Q3 */
    if (semis8 < 8u)
        return;
    step = semis8 >= 40u ? 1024 : (int32_t)(semis8 * 1024u / 40u);   /* Q10: min(1, semitones / 5) */
    for (j = 0; j <= 2u * SN_W; j++) {      /* the dip */
        uint32_t f = c - SN_W + j;
        int32_t l = sn.lv[f % SN_FQ];
        if (j < SN_W)
            hl = l > hl ? l : hl;
        else if (j > SN_W)
            hr = l > hr ? l : hr;
        if (l < lo) {
            lo = l;
            lowat = f;
        }
    }
    {
        int32_t ref = hl < hr ? hl : hr;
        if (ref > 16 && lo < ref)
            dip = (ref - lo) * 1024 / ref;
    }
    sc = dip >= 82 ? (step + dip) * 819 / 1024 : step / 2;
    thr = 819 - 614 * (int32_t)sens / 127;   /* Q10: 0.8 - 0.6 SENS / 127 */
    if (sc < thr || sn.n >= SN_MAX)
        return;
    at = sn.at[(dip >= 82 ? lowat : c) % SN_FQ];
    if (sn.n && at < sn.pos[sn.n - 1u] + sn.near) {   /* (the frames beside a step pass too: the best of them) */
        if (sc > sn.sc[sn.n - 1u]) {
            sn.pos[sn.n - 1u] = at > sn.pos[sn.n - 1u] ? at : sn.pos[sn.n - 1u];
            sn.sc[sn.n - 1u] = sc;
        }
        return;
    }
    sn.sc[sn.n] = sc;
    sn.pos[sn.n++] = at;
}

/* pass 3: the note slices of s (len samples) at sens into sn.pos (rising zero crossings) */
static void slc_notes(const slc_src_t *s, uint32_t len, uint32_t sens)
{
    slc_dec_t d;
    uint32_t src = (uint32_t)(((uint64_t)(s->rate ? s->rate : 32768u) * 44100u) >> 16);   /* the material's rate */
    uint32_t dec = (src + 2756u) / 5512u, fd, lo, hi, n = 0, fr = 0, k = 0, i;
    int32_t acc = 0, cnt = 0;
    dec = dec ? dec : 1u;
    fd = src / dec;
    lo = fd / 1100u;
    lo = lo < 2u ? 2u : lo;
    hi = fd / 46u;
    hi = hi > SN_LMAX ? SN_LMAX : hi;
    sn.n = 0;
    sn.near = 2u * SN_W * SN_FR * dec;
    slc_dec_at(&d, 0, 0);
    while (d.pos < len) {
        acc += slc_dec_next(s, &d);
        if (++cnt < (int32_t)dec)
            continue;
        sn.x[n++ & (SN_RING - 1u)] = (int16_t)(acc / cnt >> 4);
        acc = cnt = 0;
        if (n < SN_WIN + hi + 1u || (n - SN_WIN - hi - 1u) % SN_FR)
            continue;
        {                                   /* a frame: at the analysis window's middle */
            uint32_t mid = n - (SN_WIN + hi) / 2u, f = fr % SN_FQ;
            int32_t pk = 0;
            for (i = mid - SN_FR / 2u; i < mid + SN_FR / 2u; i++) {
                int32_t a = sn.x[i & (SN_RING - 1u)] < 0 ? -sn.x[i & (SN_RING - 1u)] : sn.x[i & (SN_RING - 1u)];
                pk = a > pk ? a : pk;
            }
            sn.p[f] = sn_pitch(n, lo, hi, fd * 16u);
            sn.lv[f] = pk;
            sn.at[f] = mid * dec;
            if (fr >= 2u * SN_W + 2u)
                sn_judge(fr - SN_W - 1u, sens);
            fr++;
        }
    }
    for (k = 0; k < sn.n; k++) {            /* on to the next rising zero crossing (12.5 ms at most) */
        int32_t prev;
        uint32_t lim = sn.pos[k] + src / 80u;
        slc_dec_seek(s, len, sn.pos[k], &d);
        prev = slc_dec_next(s, &d);
        while (d.pos < lim && d.pos < len) {
            int32_t x = slc_dec_next(s, &d);
            if (prev < 0 && x >= 0) {
                sn.pos[k] = d.pos - 1u;
                break;
            }
            prev = x;
        }
    }
}

/* the AUTO slices: hits (pass 2's, in s->apos), notes (sn.pos) or both, in order, SLC_MIN-like gap apart (a hit
 * beats a note within it: its attack is exact) */
static void slc_merge(slc_src_t *s, uint32_t len, uint32_t det, uint32_t gap)
{
    uint32_t hp[SLC_AUTO], nh = 0, a = 0, b = 0, last = 0, lastnote = 0;
    slc_dec_t d;
    for (a = 1; a < s->nauto; a++)
        hp[nh++] = s->apos[a];
    if (det == SLC_DET_NOTES)
        nh = 0;
    s->nauto = 1;
    a = 0;
    while ((a < nh || b < sn.n) && s->nauto < SLC_AUTO) {
        int hit = a < nh && (b >= sn.n || hp[a] <= sn.pos[b]);
        uint32_t p = hit ? hp[a++] : sn.pos[b++];
        if (p < gap)
            continue;
        if (p < last + gap) {
            if (hit && lastnote && s->nauto > 1u) {
                s->nauto--;                 /* (the note before it: the hit instead) */
            } else
                continue;
        }
        slc_dec_seek(s, len, p, &d);
        s->apos[s->nauto] = p;
        s->ast[s->nauto++] = slc_dec_st(&d);
        last = p;
        lastnote = !hit;
    }
}

/* ---- the slice table of a user slot (main loop) */
/* the grid states and the AUTO slices of s (len samples; s->len stays 0 meanwhile): see the top */
/* sens (0..127, SLC_SENS_DEF: as before SHARD's SENS): the hit thresholds x 2^((64 - sens) / 32) (x 4 at 0, about
 * x 1 / 4 at 127) and the rise a hit needs, 7 / 4 of the follower, from 11 / 4 (0) to 5 / 4 (127) */
#define SLC_SENS_DEF 64u
static void slc_scan(slc_src_t *s, uint32_t len, uint32_t sens, uint32_t det)
{
    slc_dec_t d;
    uint32_t pos = 0, k = 0, gap = ((s->rate >> 4) * 44100u >> 12) / 20u, last = 0, h0 = 0, st0 = 0;
    int32_t prev = 0, pkd = 0, pkr = 0, envd = 0, envr = 0, thd, thr, rise;
    slc_dec_at(&d, 0, 0);
    while (pos++ < len) {                               /* pass 1: the largest step and sample */
        int32_t x = slc_dec_next(s, &d), a = x - prev;
        a = a < 0 ? -a : a;
        pkd = a > pkd ? a : pkd;
        a = x < 0 ? -x : x;
        pkr = a > pkr ? a : pkr;
        prev = x;
    }
    {
        int32_t q = 64 - (int32_t)(sens > 127u ? 127u : sens), e = q >> 5, fq = 256 + 8 * (q & 31);
        fq = e >= 0 ? fq << e : fq >> -e;
        thd = (pkd / 12) * fq >> 8;
        thr = (pkr / 8) * fq >> 8;
        rise = clamp(7 + q / 16, 5, 11);
    }
    s->nauto = 1;
    s->apos[0] = 0;
    s->ast[0] = 0;
    slc_dec_at(&d, 0, 0);
    prev = 0;
    for (pos = 0; pos < len;) {                         /* pass 2: grid states, onsets */
        uint32_t hs = pos, hst = slc_dec_st(&d), e = len - pos > SLC_HOP ? pos + SLC_HOP : len;
        int32_t hd = 0, hr = 0;
        for (; pos < e; pos++) {
            int32_t x, a;
            while (k < SLC_GRID && k * len / SLC_GRID == pos)
                s->grid[k++] = slc_dec_st(&d);
            x = slc_dec_next(s, &d);
            a = x - prev;
            a = a < 0 ? -a : a;
            hd = a > hd ? a : hd;
            a = x < 0 ? -x : x;
            hr = a > hr ? a : hr;
            prev = x;
        }
        /* a rise of the high-passed peak (hats, snares, clicks) or of the peak (low hits: a slow
         * follower, so a low tone's cycles do not look like rises) */
        if (hs && ((hd > thd && hd * 4 > envd * rise) || (hr > thr && hr * 4 > envr * rise)) && h0 >= last + gap &&
            s->nauto < SLC_AUTO) {
            s->apos[s->nauto] = h0;                     /* from the hop before: pre-roll */
            s->ast[s->nauto++] = st0;
            last = h0;
        }
        envd = hd > envd ? hd : envd - envd / 16;
        envr = hr > envr ? hr : envr - envr / 64;
        h0 = hs;
        st0 = hst;
    }
    if (det == SLC_DET_NOTES || det == SLC_DET_BOTH) {   /* pass 3: the notes (DETECT) */
        slc_notes(s, len, sens);
        slc_merge(s, len, det, gap);
    }
}

/* slot k's AUTO slices again at sens (SHARD's SENS: slice_store.c at the kit's load, ui_slice.c after an edit): its
 * voices stop meanwhile, as at a scan */
static void slc_rescan(uint32_t k, uint32_t sens, uint32_t det)
{
    slc_src_t *s = &slc_usr[k % SMP_USER_SLOTS];
    uint32_t len = s->len;
    if (!len)
        return;
    s->len = 0;
    RING_PUBLISH();
    slc_scan(s, len, sens, det);
    RING_PUBLISH();
    s->len = len;
}

/* smp_user_scan (eng_sample.c): valid 0 = the slot changes (SLICE voices on it stop), 1 = it is valid.
 * The material is at most what the slot can hold (SLC_USR_MAX samples): zones over different (overlapping) data
 * past that are left out, so a crafted header cannot make the scan (at boot too) run long */
#define SLC_USR_MAX (2u * (SMP_USER_SIZE - SMP_USER_DATA))
#if FELUCCA_SHARD
static void shd_tempo_scan(uint32_t k);            /* eng_shard.c: the slot's tempo */
#define SLC_TEMPO_SCAN(k) shd_tempo_scan(k)
#else
#define SLC_TEMPO_SCAN(k) ((void)0)
#endif
static void slc_user_scan(uint32_t k, int valid)
{
    slc_src_t *s = &slc_usr[k];
    uint32_t i, j, at = 0;
    s->len = 0;
    RING_PUBLISH();
    slc_man[k][0].n = slc_man[k][1].n = 0;          /* new material: its MAN slices are gone (slc_man_load: the stored) */
    SLC_TEMPO_SCAN(k);                              /* (len 0: none) */
    if (!valid)
        return;
    s->nseg = 0;
    for (i = 0; i < usr_nz[k] && i < SLC_SEGS; i++) {
        const smp_zone_t *z = &usr_zone[k][i];
        for (j = 0; j < i && usr_zone[k][j].off != z->off; j++)
            ;
        if (j < i || !z->n)
            continue;                                   /* the same data under another key range: once */
        if (z->n > SLC_USR_MAX - at)
            break;
        s->seg[s->nseg].off = z->off;
        s->seg[s->nseg].at = at;
        s->seg[s->nseg++].n = z->n;
        at += z->n;
    }
    if (!s->nseg)
        return;
    s->rate = usr_zone[k][0].rate;
    slc_scan(s, at, SLC_SENS_DEF, SLC_DET_DEF);     /* (SHARD's SENS, DETECT, if the kit's are others: slc_rescan) */
    RING_PUBLISH();
    s->len = at;
    SLC_TEMPO_SCAN(k);
    if (slc_man_load)
        slc_man_load(k);
}

/* ---- MAN: editing (main loop, the SLICES page; based on hugelton/Felucca#27 by andreahaku). Begin with
 * slc_man_begin, change the copy it returns with move / split / join, then slc_man_commit puts it in use. */
/* the decoder state before sample pos: decoded from the grid point at or below it */
static uint32_t slc_state_at(const slc_src_t *s, uint32_t pos)
{
    slc_dec_t d;
    uint32_t k = pos * SLC_GRID / s->len;                    /* (pos < 2^18: a slot holds < 200 K samples) */
    k = k < SLC_GRID ? k : SLC_GRID - 1u;
    while (k && slc_gpos(s, k) > pos)
        k--;
    while (k + 1u < SLC_GRID && slc_gpos(s, k + 1u) <= pos)
        k++;
    slc_dec_at(&d, slc_gpos(s, k), s->grid[k]);
    while (d.pos < pos)
        slc_dec_next(s, &d);
    return slc_dec_st(&d);
}

/* the copy to edit for user slot k: the MAN slices in use, or the AUTO ones when there are none (those that keep
 * SLC_MIN apart and from the end); 0 = no material in the slot */
static slc_man_t *slc_man_begin(uint32_t k)
{
    const slc_src_t *s = k < SMP_USER_SLOTS ? slc_get(slc_src_of_slot(k)) : 0;
    const slc_man_t *cur;
    slc_man_t *m;
    uint32_t i;
    if (!s)
        return 0;
    cur = &slc_man[k][slc_man_cur[k] & 1u];
    m = &slc_man[k][(slc_man_cur[k] & 1u) ^ 1u];
    if (cur->n) {
        *m = *cur;
        return m;
    }
    m->n = 0;
    m->end = 0;
    for (i = 0; i < s->nauto; i++)
        if (s->apos[i] < s->len && s->len - s->apos[i] >= SLC_MIN &&
            (!m->n || s->apos[i] - m->pos[m->n - 1u] >= SLC_MIN)) {
            m->pos[m->n] = s->apos[i];
            m->st[m->n++] = s->ast[i];
        }
    return m;
}
static void slc_man_commit(uint32_t k)
{
    RING_PUBLISH();
    slc_man_cur[k] ^= 1u;
}

/* move the start of slice j by d samples (slice 0's start trims the material's head); keeps SLC_MIN to both
 * neighbours. Returns the new start. */
static uint32_t slc_man_move(const slc_src_t *s, slc_man_t *m, uint32_t j, int32_t d)
{
    int32_t lo, hi, p;
    if (j >= m->n)
        return 0u;
    lo = j ? (int32_t)(m->pos[j - 1u] + SLC_MIN) : 0;
    hi = (int32_t)(j + 1u < m->n ? m->pos[j + 1u] : slc_man_end(s, m)) - (int32_t)SLC_MIN;
    if (hi < lo)                                    /* no room (a very short slice pair): it stays */
        return m->pos[j];
    p = clamp((int32_t)m->pos[j] + d, lo, hi);
    if (p != (int32_t)m->pos[j]) {
        m->pos[j] = (uint32_t)p;
        m->st[j] = slc_state_at(s, (uint32_t)p);
    }
    return m->pos[j];
}

/* move the end of all slices (the last slice's end: a tail trim) by d samples, at most the material's end */
static uint32_t slc_man_move_end(const slc_src_t *s, slc_man_t *m, int32_t d)
{
    if (!m->n)
        return 0u;
    if (m->pos[m->n - 1u] + SLC_MIN <= s->len)       /* (else no room: the end stays) */
        m->end = (uint32_t)clamp((int32_t)slc_man_end(s, m) + d, (int32_t)(m->pos[m->n - 1u] + SLC_MIN), (int32_t)s->len);
    return slc_man_end(s, m);
}

/* split slice j in two at its middle; returns the new slice (j + 1), or j if it cannot (32 slices, too short) */
static uint32_t slc_man_split(const slc_src_t *s, slc_man_t *m, uint32_t j)
{
    uint32_t a, b, i;
    if (j >= m->n || m->n >= SLC_AUTO)
        return j;
    a = m->pos[j];
    b = j + 1u < m->n ? m->pos[j + 1u] : slc_man_end(s, m);
    if (b - a < 2u * SLC_MIN)
        return j;
    for (i = m->n; i > j + 1u; i--) {
        m->pos[i] = m->pos[i - 1u];
        m->st[i] = m->st[i - 1u];
    }
    m->pos[j + 1u] = a + (b - a) / 2u;
    m->st[j + 1u] = slc_state_at(s, m->pos[j + 1u]);
    m->n++;
    return j + 1u;
}

/* join slice j (1 .. n - 1) to the one before (its start goes); returns that one */
static uint32_t slc_man_join(slc_man_t *m, uint32_t j)
{
    uint32_t i;
    if (!j || j >= m->n)
        return j < m->n ? j : 0u;
    for (i = j; i + 1u < m->n; i++) {
        m->pos[i] = m->pos[i + 1u];
        m->st[i] = m->st[i + 1u];
    }
    m->n--;
    return j - 1u;
}

/* put stored slices in use for user slot k (n starts pos[], end; 0 = the material's end); 0 = done, -1 = they do
 * not fit its material (nothing changes). Junk is refused: no sums that can wrap */
static int slc_man_restore(uint32_t k, uint32_t n, uint32_t end, const uint32_t *pos)
{
    const slc_src_t *s = k < SMP_USER_SLOTS ? slc_get(slc_src_of_slot(k)) : 0;
    slc_man_t *m;
    uint32_t i, e;
    if (!s || !n || n > SLC_AUTO || end > s->len)
        return -1;
    e = end ? end : s->len;
    for (i = 0; i < n; i++)
        if (pos[i] >= e || e - pos[i] < SLC_MIN || (i && pos[i] < pos[i - 1u]) || (i && pos[i] - pos[i - 1u] < SLC_MIN))
            return -1;
    m = slc_man_begin(k);
    m->n = n;
    m->end = end;
    for (i = 0; i < n; i++) {
        m->pos[i] = pos[i];
        m->st[i] = slc_state_at(s, pos[i]);
    }
    slc_man_commit(k);
    return 0;
}

/* ---- voices */
static int16_t *slc_rb(track_t *t, voice_t *v)
{
    uint32_t p = (uint32_t)(t - trk), i = (uint32_t)(v - t->v);
    return p < NPART && i < NVOICE ? (*(slc_rbuf_t *)ENG_MEM(p))[i] : 0;
}

static void slice_note_on(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    uint32_t src = slc_src_of(p), div = (uint32_t)clamp(p[P_E1], 0, SLC_DIV_MAN), rev = p[P_E5] != 0;
    uint32_t a, b, st, j;
    const slc_src_t *s = slc_get(src);
    v->ph[1] = 0;
    v->s[2] = v->s[3] = 0;
    v->s[5] = 32767;
    v->s[6] = 2;
    v->s[7] = 0;
    v->env_out = 0;                                     /* a new slice fades in over one block */
    if (!s) {                                           /* no material: the sine (from phase 0: no click) */
        v->s[4] = (int32_t)SLC_SINE;
        v->s[6] = 1;
        v->ph[0] = 0;
        return;
    }
    j = slc_note_slice(p, v->note, slc_count(s, div));
    slc_bounds(s, div, j, &a, &b, &st);
    if (b <= a || (rev && !slc_rb(t, v)))
        return;
    v->s[4] = (int32_t)(src | (st >> 24) << 3 | rev << 7 | j << 8 | div << 16);
    v->s[6] = 0;
    if (rev) {
        v->ph[0] = b;
        v->ph[2] = a;
        v->s[0] = 0x7FFFFFFF;                           /* no window yet */
    } else {
        v->ph[0] = a;
        v->ph[2] = b;
        v->s[0] = (int16_t)(st & 0xFFFFu);
        v->s[1] = (int32_t)((st >> 16) & 127u);
    }
}

/* the reverse window [ws, ws + n): decoded from the nearest checkpoint at or below ws (a grid point,
 * or a = the slice start with its state sa) */
static void slc_fill(const slc_src_t *s, int16_t *rb, uint32_t ws, uint32_t n, uint32_t a, uint32_t sa)
{
    slc_dec_t d;
    uint32_t k = ws * SLC_GRID / s->len, i;
    while (k + 1u < SLC_GRID && slc_gpos(s, k + 1u) <= ws)
        k++;
    if (a <= ws && a > slc_gpos(s, k))
        slc_dec_at(&d, a, sa);
    else
        slc_dec_at(&d, slc_gpos(s, k), s->grid[k]);
    while (d.pos < ws)
        slc_dec_next(s, &d);
    for (i = 0; i < n; i++)
        rb[i] = (int16_t)slc_dec_next(s, &d);
}

/* LOOP: back to the slice's start; its end again too (MAN: a JOIN / move meanwhile changed it). Out of line: once
 * per loop, it keeps slc_fwd inlined in slice_render */
static __attribute__((noinline)) void slc_loop_fwd(const slc_src_t *s, voice_t *v, slc_dec_t *d)
{
    uint32_t a, b, st;
    slc_bounds_v(s, v, &a, &b, &st);
    slc_dec_at(d, a, st);
    v->ph[2] = b;
}
/* the next source sample into *x; 0 = the slice ended */
static inline int slc_fwd(const slc_src_t *s, voice_t *v, slc_dec_t *d, int loop, int32_t *x)
{
    if (d->pos >= v->ph[2]) {
        if (!loop)
            return 0;
        slc_loop_fwd(s, v, d);
    }
    *x = slc_dec_next(s, d);
    return 1;
}
static inline int slc_rev(const slc_src_t *s, voice_t *v, int16_t *rb, int loop, int32_t *x)
{
    uint32_t q = v->ph[0], ws = (uint32_t)v->s[0], a, b, st;
    if (q <= v->ph[2]) {
        if (!loop)
            return 0;
        slc_bounds_v(s, v, &a, &b, &st);
        q = b;
        v->ph[2] = a;                                   /* (MAN: a JOIN / move meanwhile changed the start too) */
    }
    q--;
    if (q < ws || q >= ws + SLC_RB) {
        slc_bounds_v(s, v, &a, &b, &st);
        if (q < a)                                      /* MAN: the start moved past this voice: it ends */
            return 0;
        ws = q + 1u >= a + SLC_RB ? q + 1u - SLC_RB : a;
        slc_fill(s, rb, ws, q + 1u - ws, a, st);
        v->s[0] = (int32_t)ws;
    }
    *x = rb[q - ws];
    v->ph[0] = q;
    return 1;
}

/* the ADSR (ONE: no release), DECAY, the fade at the slice end */
static int32_t slice_amp(track_t *t, voice_t *v, int32_t adsr)
{
    const int16_t *p = t->p;
    if (p[P_E4] == SLC_ONE && v->stage == 3u && ((uint32_t)v->s[4] & 7u) != SLC_SINE)
        v->stage = 2;                                   /* ONE: the slice plays to its end (the sine has none) */
    if (p[P_E6] < 127) {
        v->s[5] -= mulq16(v->s[5], ENV_EXP[p[P_E6] & 127]);
        if (v->s[5] < 8)
            v->s[6] = 2;                                /* faded out: the voice ends */
    }
    return v->s[6] == 3 ? 0 : mulq15(adsr, v->s[5]);
}

static void slice_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t pk = (uint32_t)v->s[4], i, frac = v->ph[1], rev = (pk >> 7) & 1u, stepq, rem;
    int32_t lp = 4000 + ((clamp((p[P_E7] << 8) + m->cutoff, 0, 127 << 8) * 28767) >> 15), x;
    int loop = p[P_E4] == SLC_LOOP && v->gate;
    const slc_src_t *s = v->s[6] == 2 || (pk & 7u) == SLC_SINE ? 0 : slc_get(pk & 7u);
    int16_t *rb = slc_rb(t, v);
    slc_dec_t d;
    if ((pk & 7u) == SLC_SINE && v->s[6] != 2) {        /* no material: the sine at the key's pitch + PTCH */
        smp_sine(out, n, m, p[P_E3] * 16, lp, &v->ph[0], &v->s[7]);
        return;
    }
    if (!s) {                                           /* ended, or its slot was erased */
        v->active = 0;
        return;
    }
    stepq = (pow2_q16(clamp(m->pitch16 - v->pitch_cur + p[P_E3] * 16, -1536, 576)) >> 8) * (s->rate >> 8);
    d.pos = v->ph[0];
    d.pred = v->s[0];
    d.idx = v->s[1];
    d.seg = (pk >> 3) & 15u;
    if (!v->s[6]) {                                     /* prime the interpolator */
        if (!(rev ? slc_rev(s, v, rb, 0, &x) : slc_fwd(s, v, &d, 0, &x))) {
            v->active = 0;
            return;
        }
        v->s[3] = x;
        v->s[6] = 1;
    }
    rem = rev ? v->ph[0] - v->ph[2] : v->ph[2] - d.pos;
    if (v->s[6] == 1 && !loop && rem < ((stepq * 3u * n) >> 16))
        v->s[6] = 3;                                    /* the next block fades to 0 (slice_amp) */
    for (i = 0; i < n; i++) {
        int32_t y;
        frac += stepq;
        while (frac >= 65536u) {
            frac -= 65536u;
            v->s[2] = v->s[3];
            if (!(rev ? slc_rev(s, v, rb, loop, &x) : slc_fwd(s, v, &d, loop, &x))) {
                v->s[6] = 2;                            /* the slice ended */
                v->s[3] = 0;
                break;
            }
            v->s[3] = x;
        }
        y = v->s[2] + (((v->s[3] - v->s[2]) * (int32_t)(frac >> 1)) >> 15);
        v->s[7] += mulq15(y - v->s[7], lp);             /* TONE */
        out[i] += voice_amp(v->s[7], m, i) << 1;
        if (v->s[6] == 2)
            break;
    }
    v->ph[1] = frac;
    if (!rev) {
        v->ph[0] = d.pos;
        v->s[0] = d.pred;
        v->s[1] = d.idx;
        v->s[4] = (int32_t)((pk & ~(15u << 3)) | d.seg << 3);
    }
}

/* two factory sounds, both on BREAK (always there). PATTERNS (engines.c): CHOP suggests
 * 9 CHOP (16 slices re-ordered), STUTTER 10 STUTTER (8 slices, repeats); 11 SLICES plays 0..15 in order */
static const preset_t SLICE_PRESETS[] = {
    {"CHOP", {0, 2, 0, 0, SLC_ONE, 0, 127, 127}, {0, 127, 127, 30}, 0, 0, FX(0, 0, 0, 12), PAT(9)},
    {"STUTTER", {0, 1, 0, 0, SLC_GATE, 0, 90, 110}, {0, 127, 127, 12}, 0, 0, FX(10, 0, 20, 10), PAT(10)},
};

/* the lowest key is slice 0 (C4 + ROOT), no scale */
static int32_t slice_keys(const track_t *t, uint32_t k)
{
    return clamp(SLC_BASE + t->p[P_ROOT] + 12 * song.octave + (int32_t)k, 0, 127);
}

static const engine_t ENG_SLICE = {
    .name = "SLICE",
    .page_title = {"SLCE", "PLAY"},
    .edit = {
        {"SRC", F_ENUM, 0, SLC_NSRC - 1, 0, N_SLC_SRC, 0},
        {"DIV", F_ENUM, 0, 5, 2, N_SLC_DIV, 0},
        {"START", F_INT, 0, 31, 0, 0, 0},
        {"PTCH", F_SEMI, -24, 24, 0, 0, 0},
        {"MODE", F_ENUM, 0, 2, 0, N_SLC_MODE, 0},
        {"REV", F_ENUM, 0, 1, 0, N_SLC_REV, 0},
        {"DCAY", F_TIME, 0, 127, 127, 0, 0},
        {"TONE", F_INT, 0, 127, 127, 0, 0},
    },
    .presets = SLICE_PRESETS,
    .npresets = NELEM(SLICE_PRESETS),
    .note_on = slice_note_on,
    .render = slice_render,
    .knob = {P_E0, P_E1, P_E3, P_E6},
    .sampled = 1,
    .keys = slice_keys,
    .amp = slice_amp,
    .mem = sizeof(slc_rbuf_t),
};

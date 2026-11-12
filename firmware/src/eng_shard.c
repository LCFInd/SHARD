/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD: a slice-loop engine with per-slice sound design and an Ohmnicide-style multiband distortion.
 *
 * Built on SLICE (eng_slice.c): the same material (SRC: BREAK, USR1..3, PIANO), the same slices (DIV: 2 4 6 8 12 16 32
 * equal, AUTO, MAN from the SLICES page) and the same IMA ADPCM decoder, checkpoints and reverse windows. What SHARD adds:
 *
 * Per-slice settings (the "kit", one per source, kept with a user slot's sample: shard_store in slice_store.c):
 *   PITCH (-24..+24 st), LEVEL, ATTACK, DECAY, SUSTAIN, RELEASE (its own envelope, restarted by each slice; RELEASE
 *   OFF leaves the key's release to the track's envelope), MODE (TRACK / ONE / GATE / LOOP), DIR (FWD, REV, PING:
 *   forward then back, once or, looping, to and fro), FILTER (OFF / LP / HP / BP, a 2x state-variable filter;
 *   COMB- / COMB+, a feedback comb tuned by CUT) with CUT and RES, DRIVE (OFF OVRDRV DIST FOLD CRUSH DECIM HALF TUBE
 *   FUZZ) with AMT, DELAY (a send into Felucca's tempo delay, beside the track's), ROUTE and CHOKE (1..4: a slice fades
 *   the part's other slices of its group).
 * ROUTE: DIRECT to the part as any engine, or into the part's MULTI bus (split low / mid / high), or straight into
 *   one of the four channels: a band's shaper (LOW / MID / HIGH) or FULL, a full-range one (clean as it starts). The
 *   bus (shd_post, the engine's post hook: per part, after its voices) runs four shapers, a gate on each, their
 *   levels, a DC blocker, and the dry / wet MULTI mix (E6). The channels' types, drives, gates, levels and the two
 *   split points are the kit's too; DRIVE (E7) offsets every channel's drive. SENS (the kit's) sets how readily
 *   AUTO finds hits (eng_slice.c slc_scan).
 * MODE PLAY (E4), ReCycle style: a note plays the whole loop from its slice, each slice starting where it sits in the
 *   loop, scaled to the tempo (BARS: the loop's length; AUTO: from the source's tempo, the kit's BPM as set on SLICE
 *   FX's BUS or else the one shd_tempo_scan found, and failing both the bars that put it in 80..160 BPM). A
 *   tempo change moves the slices, never the pitch; slices that end early leave a gap (DECAY / LOOP fill it). Each
 *   slice keeps its own settings. The loop repeats while the note is held.
 *
 * Per-voice state beyond voice_t (the part's engine memory, ENG_MEM: reverse windows, filter, envelope, PLAY's clock)
 * and the bus. The voice_t fields as SLICE uses them (eng_slice.c), so its decoder helpers work on SHARD's voices. */
#include "shard_tables.h"

static uint32_t beat_samples(void);               /* (fx.c: the song tempo or the external clock's) */

enum { SHD_TRACK, SHD_ONE, SHD_GATE, SHD_LOOP };   /* a slice's MODE (TRACK: the track's E4) */
enum { SHD_M_ONE, SHD_M_GATE, SHD_M_LOOP, SHD_M_PLAY };   /* the track's MODE (E4) */
enum { SHD_DIRECT, SHD_MULTI, SHD_LOW, SHD_MID, SHD_HIGH, SHD_FULL, SHD_NROUTE };
#define SHD_NCH 4u                                  /* the channels: LOW MID HIGH FULL */
enum { SHD_D_OFF, SHD_D_SOFT, SHD_D_HARD, SHD_D_FOLD, SHD_D_CRUSH, SHD_D_DECIM, SHD_D_HALF, SHD_D_TUBE, SHD_D_FUZZ,
       SHD_NDRIVE };
enum { SHD_F_OFF, SHD_F_LP, SHD_F_HP, SHD_F_BP, SHD_F_COMBN, SHD_F_COMBP, SHD_NFILT };
enum { SHD_FWD, SHD_REV, SHD_PING, SHD_NDIR };
#define SHD_NSRC SLC_NSRC
#define SHD_BPM_MIN 40u                             /* the kit's BPM (SLICE FX: BUS), 0 = AUTO */
#define SHD_BPM_MAX 250u
#define SHD_SENS_DEF 64u                            /* SENS: slc_scan as SLICE's */
#define SHD_COMB 512u                               /* comb filters: samples of line per voice (90 Hz) */
#define SHD_FADE 32u                                /* PLAY: samples of fade at a slice change */

static const char *const N_SHD_MODE[] = {"ONE", "GATE", "LOOP", "PLAY"};
static const char *const N_SHD_BARS[] = {"AUTO", "1", "2", "4", "8"};
/* DIV (E1): SLICE's codes (eng_slice.c SLC_DIV_*) in the order of the slice counts */
static const char *const N_SHD_DIV[] = {"2", "4", "6", "8", "12", "16", "32", "AUTO", "MAN"};
static const uint8_t SHD_DIV_CODE[] = {SLC_DIV_2, 0, SLC_DIV_6, 1, SLC_DIV_12, 2, 3, SLC_DIV_AUTO, SLC_DIV_MAN};
#define SHD_DIV_AUTO 7
#define SHD_DIV_MAN 8
#define SHD_NDIV 9
static uint32_t shd_div(const int16_t *p) { return SHD_DIV_CODE[clamp(p[P_E1], 0, SHD_NDIV - 1)]; }
static const char *const N_SHD_SMODE[] = {"TRACK", "ONE", "GATE", "LOOP"};
static const char *const N_SHD_FILT[] = {"OFF", "LP", "HP", "BP", "COMB-", "COMB+"};
static const char *const N_SHD_DIR[] = {"FWD", "REV", "PING"};
static const char *const N_SHD_DRIVE[] = {"OFF", "OVRDRV", "DIST", "FOLD", "CRUSH", "DECIM", "HALF", "TUBE", "FUZZ"};
static const char *const N_SHD_ROUTE[] = {"DIRECT", "MULTI", "LOW", "MID", "HIGH", "FULL"};
static const char *const N_SHD_CHOKE[] = {"OFF", "1", "2", "3", "4"};

/* ---- the kit: per source, per slice (SLC_AUTO = 32 at most), and the bus */
typedef struct {
    int8_t pitch;
    uint8_t level, atk, dec, mode, rev, filt, cut, res, dtype, drv, route, choke;
    uint8_t sus, rel, dly;                          /* SUSTAIN, RELEASE (0: OFF, the track's), DELAY's send */
} shd_sl_t;
typedef struct {
    uint8_t lo, hi;                                 /* split points: 40 Hz .. 12 kHz */
    uint8_t type[SHD_NCH], drive[SHD_NCH], gate[SHD_NCH], level[SHD_NCH];   /* low, mid, high, full */
    uint8_t bpm;                                    /* the source's tempo (40..250), 0: AUTO (shd_tempo_scan) */
    uint8_t sens;                                   /* AUTO's hits: 0 fewest .. 127 most (SHD_SENS_DEF: SLICE's) */
    uint8_t pad[4];
} shd_bus_t;
typedef struct {
    shd_sl_t sl[SLC_AUTO];
    shd_bus_t bus;
} shd_kit_t;
_Static_assert(sizeof(shd_sl_t) == 16u && sizeof(shd_bus_t) == 24u, "SHARD kit layout");
/* neutral at power-on (the slot scans then read the stored kits): every slice as recorded, DIRECT */
#define SHD_SL_DEF {0, 100, 0, 127, SHD_TRACK, SHD_FWD, SHD_F_OFF, 127, 0, SHD_D_OFF, 40, SHD_DIRECT, 0, 0, 0, 0}
#define SHD_BUS_DEF {51, 96, {SHD_D_TUBE, SHD_D_FOLD, SHD_D_CRUSH, SHD_D_OFF}, {40, 30, 60, 40}, {0, 0, 0, 0}, \
                     {100, 100, 90, 127}, 0, SHD_SENS_DEF, {0, 0, 0, 0}}
#define SHD_KIT_DEF {{[0 ... SLC_AUTO - 1] = SHD_SL_DEF}, SHD_BUS_DEF}
static shd_kit_t shd_kit[SHD_NSRC] = {[0 ... SHD_NSRC - 1] = SHD_KIT_DEF};
static uint8_t shd_kit_save;                        /* bit k: user slot k's kit changed, to flash (slice_store.c) */

static void shd_sl_default(shd_sl_t *s)
{
    *s = (shd_sl_t)SHD_SL_DEF;
}
static void shd_bus_default(shd_bus_t *b)
{
    *b = (shd_bus_t)SHD_BUS_DEF;
}
static void shd_kit_default(shd_kit_t *k)
{
    uint32_t i;
    for (i = 0; i < SLC_AUTO; i++)
        shd_sl_default(&k->sl[i]);
    shd_bus_default(&k->bus);
}
static void shd_init(void)                          /* main.c (boot): every kit neutral */
{
    uint32_t i;
    for (i = 0; i < SHD_NSRC; i++)
        shd_kit_default(&shd_kit[i]);
}
/* a stored kit's values in range (junk refused field by field: a value out of range reads as its default) */
static void shd_kit_sane(shd_kit_t *k)
{
    uint32_t i, b;
    shd_sl_t d;
    shd_bus_t db;
    shd_sl_default(&d);
    shd_bus_default(&db);
    for (i = 0; i < SLC_AUTO; i++) {
        shd_sl_t *s = &k->sl[i];
        if (s->pitch < -24 || s->pitch > 24) s->pitch = 0;
        if (s->level > 127) s->level = d.level;
        if (s->atk > 127) s->atk = d.atk;
        if (s->dec > 127) s->dec = d.dec;
        if (s->mode > SHD_LOOP) s->mode = SHD_TRACK;
        if (s->rev >= SHD_NDIR) s->rev = SHD_FWD;
        if (s->filt >= SHD_NFILT) s->filt = SHD_F_OFF;
        if (s->cut > 127) s->cut = d.cut;
        if (s->res > 127) s->res = 0;
        if (s->dtype >= SHD_NDRIVE) s->dtype = SHD_D_OFF;
        if (s->drv > 127) s->drv = d.drv;
        if (s->route >= SHD_NROUTE) s->route = SHD_DIRECT;
        if (s->choke > 4) s->choke = 0;
        if (s->sus > 127) s->sus = 0;
        if (s->rel > 127) s->rel = 0;
        if (s->dly > 127) s->dly = 0;
    }
    if (k->bus.lo > 127) k->bus.lo = db.lo;
    if (k->bus.hi > 127) k->bus.hi = db.hi;
    for (b = 0; b < SHD_NCH; b++) {
        if (k->bus.type[b] >= SHD_NDRIVE) k->bus.type[b] = db.type[b];
        if (k->bus.drive[b] > 127) k->bus.drive[b] = db.drive[b];
        if (k->bus.gate[b] > 127) k->bus.gate[b] = 0;
        if (k->bus.level[b] > 127) k->bus.level[b] = db.level[b];
    }
    if (k->bus.bpm && (k->bus.bpm < SHD_BPM_MIN || k->bus.bpm > SHD_BPM_MAX)) k->bus.bpm = 0;
    if (k->bus.sens > 127) k->bus.sens = SHD_SENS_DEF;
}

/* ---- the part's engine memory */
typedef struct {
    int32_t lo, bd;                                 /* the filter */
    int32_t held;                                   /* DECIM's sample */
    uint32_t env;                                   /* the slice's envelope, 0 .. 1 << 30 */
    uint32_t clock, next, loop;                     /* PLAY: samples since the note, the next slice's start, the loop */
    uint16_t hcnt, fadein, cw;                      /* (cw: the comb line's write point) */
    uint8_t stage;                                  /* the slice's envelope: 0 attack, 1 decay, 2 sustain, 3 release */
    uint8_t first, idle, choked;                    /* PLAY: the note's slice; waiting for the next slice; choked */
    uint8_t pinged;                                 /* DIR PING: turned at least once */
} shd_vx_t;
typedef struct {
    int16_t rb[NVOICE][SLC_RB];                     /* reverse windows (as SLICE's) */
    shd_vx_t vx[NVOICE];
    int16_t comb[NVOICE][SHD_COMB];                 /* the comb filters' lines */
    int32_t bus[1u + SHD_NCH][CTL];                 /* MULTI, LOW, MID, HIGH, FULL: this block */
    int32_t dsend[CTL];                             /* DELAY: the slices' send this block (shard_send) */
    uint32_t used, tail, dused;                     /* buses written this block; blocks of tail left; dsend written */
    int32_t lpl, lph, dcx, dcy;                     /* the split, the DC blocker */
    int32_t fol[SHD_NCH], gain[SHD_NCH], held[SHD_NCH];
    uint16_t cnt[SHD_NCH];
} shd_mem_t;
ENG_MEM_FITS("SHARD", sizeof(shd_mem_t));
static shd_mem_t *shd_mem(const track_t *t) { return (shd_mem_t *)ENG_MEM((uint32_t)(t - trk) % NPART); }
static shd_vx_t *shd_vx(track_t *t, voice_t *v) { return &shd_mem(t)->vx[(uint32_t)(v - t->v) % NVOICE]; }

/* the slice a voice plays, and its settings */
static uint32_t shd_slice_of(const voice_t *v) { return ((uint32_t)v->s[4] >> 8) & 63u; }
static const shd_sl_t *shd_sl(const voice_t *v)
{
    uint32_t pk = (uint32_t)v->s[4];
    return &shd_kit[(pk & 7u) < SHD_NSRC ? pk & 7u : 0u].sl[((pk >> 8) & 63u) % SLC_AUTO];
}
/* the mode a slice plays in: its own, or the track's */
static uint32_t shd_mode(const int16_t *p, const shd_sl_t *sl)
{
    uint32_t tm = (uint32_t)clamp(p[P_E4], 0, SHD_M_PLAY);
    if (tm == SHD_M_PLAY)
        return SHD_M_PLAY;
    return sl->mode ? sl->mode - 1u : tm;
}

/* ---- shaping (Q15 in, Q15 out) */
static inline int32_t shd_sat(int32_t x, int32_t l) { return x > l ? l : x < -l ? -l : x; }
static int32_t shd_soft(int32_t x)                  /* tanh, rational: exact to ~2 % */
{
    int32_t p, p2;
    if (x >= 3 * 32768)
        return 32767;
    if (x <= -3 * 32768)
        return -32767;
    p = x >> 3;
    p2 = (p * p) >> 12;
    return p * (27 * 4096 + p2) / ((27 * 4096 + 9 * p2) >> 3);
}
static int32_t shd_shape(uint32_t type, int32_t x, uint32_t drive, int32_t *held, uint16_t *cnt)
{
    int32_t p, y;
    drive = drive > 127u ? 127u : drive;
    p = (int32_t)(((int64_t)shd_sat(x, 1 << 17) * SHD_DRIVE_Q8[drive]) >> 8);
    switch (type) {
    case SHD_D_SOFT: y = shd_soft(p); break;
    case SHD_D_HARD: y = shd_sat(p, 32767); break;
    case SHD_D_FOLD: {
        int32_t f = (shd_sat(p, 1 << 22) + 32768) & 131071;
        y = f < 65536 ? f - 32768 : 98304 - f;
        break;
    }
    case SHD_D_CRUSH: {
        uint32_t sh = 1u + drive * 12u / 127u;
        return (shd_sat(x, 32767) >> sh) * (1 << sh);
    }
    case SHD_D_DECIM:
        if (++*cnt >= 1u + drive * 31u / 127u) {
            *cnt = 0;
            *held = x;
        }
        return *held;
    case SHD_D_HALF: y = p > 0 ? shd_soft(p) : shd_soft(p) >> 2; break;
    case SHD_D_TUBE: y = p >= 0 ? shd_soft(p) : (shd_soft(p + (p >> 1)) * 3) >> 2; break;
    case SHD_D_FUZZ: y = shd_sat(shd_soft(shd_sat(p, 1 << 23) * 4), 26000) * 5 / 4; break;   /* near square */
    default: return x;
    }
    return (y * SHD_MAKEUP[drive]) >> 15;
}

/* ---- the source's tempo. shd_tempo_scan (main loop: a user slot's scan, after slc_scan) finds the tempo of a rhythmic
 * sample: per hop (~7 ms) the rise of its level and of its first difference (hits, as slc_scan's onsets), as a
 * byte per hop (at most SHD_TF hops: longer hops for a longer sample); the autocorrelation of that onset curve; for
 * each tempo 60..200 BPM in 0.5 BPM steps the score at the beat (interpolated between whole lags) plus half the score
 * at two beats, x 7/8 outside 80..160 (the half- and double-time doubt goes to the usual range). The best tempo is
 * kept when it stands out (its score 1.2 x the mean of all); then, if the sample is within 3 % of a whole number of
 * beats at it, it is set to exactly that (a loop cut on the beat, its usual case, repeats seamlessly in PLAY): a
 * multiple of 4 beats first, then 2, then any. 0: no tempo found (silence, a single hit, a pad). RAM: SHD_TF bytes. */
#define SHD_TF 1024u
static uint16_t shd_det_bpm10[SHD_NSRC] = {1200, 0, 0, 0, 0};   /* found, in 0.1 BPM (BREAK: one bar at 120) */
static uint8_t shd_onset[SHD_TF];
static uint32_t shd_src_hz(const slc_src_t *s) { return (s->rate >> 4) * 44100u >> 12; }

/* the onset curve's circular autocorrelation (a loop repeats: the end meets the start) less its mean squared, at lag
 * l16 / 16 hops (linear between whole lags), per hop */
static int32_t shd_acf(const uint8_t *o, uint32_t n, uint32_t l16, int32_t mean)
{
    uint32_t l = (l16 >> 4) % n, f = l16 & 15u, i, j = l, k = (l + 1u) % n;
    int32_t a = 0, b = 0;
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)o[i] - mean;
        a += x * ((int32_t)o[j] - mean);
        b += x * ((int32_t)o[k] - mean);
        j = j + 1u == n ? 0u : j + 1u;
        k = k + 1u == n ? 0u : k + 1u;
    }
    a /= (int32_t)n;
    b /= (int32_t)n;
    return (a * (int32_t)(16u - f) + b * (int32_t)f) / 16;
}
/* a tempo's score: the beat, two beats, four beats (a bar) and the half beat, from the curve's covariance */
static int32_t shd_beat_score(const uint8_t *o, uint32_t n, uint32_t l16, int32_t mean)
{
    return shd_acf(o, n, l16, mean) + shd_acf(o, n, 2u * l16, mean) + shd_acf(o, n, 4u * l16, mean) / 2 +
           shd_acf(o, n, l16 / 2u, mean) / 2;
}

static uint32_t shd_tempo_of(const slc_src_t *s, uint8_t *o)
{
    slc_dec_t d;
    uint32_t len = s->len, hz = shd_src_hz(s), hop, n, i, pos, pass, top = 1, best = 0, b10, sum = 0;
    int32_t prev = 0, ed = 0, er = 0, mean, var, best_s = -0x7FFFFFFF, lvl = 0;
    if (!len || !hz)
        return 0;
    hop = hz / 150u;                                    /* ~6.7 ms */
    if (len / hop > SHD_TF)
        hop = (len + SHD_TF - 1u) / SHD_TF;
    n = len / hop;
    if (n < 32u)
        return 0;
    for (pass = 0; pass < 2u; pass++) {                 /* 1: the largest rise; 2: the curve, scaled to it */
        slc_dec_at(&d, 0, 0);
        prev = ed = er = 0;
        for (i = 0; i < n; i++) {
            int32_t hd = 0, hr = 0, r;
            for (pos = 0; pos < hop; pos++) {
                int32_t x = slc_dec_next(s, &d), a = x - prev;
                prev = x;
                hd += a < 0 ? -a : a;
                hr += x < 0 ? -x : x;
            }
            hd /= (int32_t)hop;
            hr /= (int32_t)hop;
            r = (hd > ed ? hd - ed : 0) + (hr > er ? (hr - er) / 4 : 0);
            ed = hd;
            er = hr;
            if (!pass) {
                top = (uint32_t)r > top ? (uint32_t)r : top;
                lvl = hr > lvl ? hr : lvl;
            } else {                                    /* square-root compressed: soft hits count, loud ones less */
                uint32_t q = (uint32_t)r * 65025u / top, z = 0, bit;
                for (bit = 1u << 14; bit; bit >>= 1)
                    if ((z | bit) * (z | bit) <= q)
                        z |= bit;
                o[i] = (uint8_t)(z > 255u ? 255u : z);
                sum += o[i];
            }
        }
        if (!pass && (int32_t)top * 16 < lvl)        /* no rise of note against its level: no hits (a held tone) */
            return 0;
    }
    mean = (int32_t)(sum / n);
    var = shd_acf(o, n, 0, mean);
    if (var <= 0)
        return 0;
    for (b10 = 600; b10 <= 2000u; b10 += 5u) {         /* the beat: 600 * hops per s / b10 hops (x 16) */
        uint32_t l16 = (uint32_t)((uint64_t)hz * 16u * 600u / ((uint64_t)hop * b10));
        int32_t sc = shd_beat_score(o, n, l16, mean);
        if (b10 < 800u || b10 > 1600u)
            sc -= (sc > 0 ? sc : -sc) / 8;
        if (sc > best_s) {
            best_s = sc;
            best = b10;
        }
    }
    if (!best || best_s * 4 < var * 3)                  /* (3 of the 3.5 a perfect pulse scores: under 1 in 4 of it) */
        return 0;
    {   /* a whole number of beats, if the sample is within 3 % of one: exactly that */
        static const uint8_t M[3] = {4, 2, 1};
        uint64_t beats1000 = (uint64_t)len * best * 1000u / (600u * hz);   /* the sample's beats x 1000 */
        uint32_t m;
        for (m = 0; m < 3u; m++) {
            uint64_t q = M[m] * 1000u, k = (beats1000 + q / 2u) / q * q;
            if (k && (beats1000 > k ? beats1000 - k : k - beats1000) * 100u <= 3u * k) {
                uint32_t exact = (uint32_t)((k / 1000u) * 600u * hz * 10u / len);   /* x 10: 0.01 BPM */
                if (exact >= 6000u && exact <= 20000u)
                    return (exact + 5u) / 10u;
            }
        }
    }
    return best;
}

/* smp_user_scan (eng_sample.c, via slc_user_scan): slot k's tempo; none while it has no material */
static void shd_tempo_scan(uint32_t k)
{
    if (k < SMP_USER_SLOTS)
        shd_det_bpm10[slc_src_of_slot(k)] = (uint16_t)shd_tempo_of(&slc_usr[k], shd_onset);
}
/* the source's tempo in 0.1 BPM: the kit's, else the one found; 0 = unknown */
static uint32_t shd_bpm10(uint32_t src)
{
    if (src >= SHD_NSRC)
        return 0;
    return shd_kit[src].bus.bpm ? shd_kit[src].bus.bpm * 10u : shd_det_bpm10[src];
}

/* ---- PLAY: the loop's length and where slice k starts in it (samples), from the note's slice f */
static uint32_t shd_bars(const int16_t *p, const slc_src_t *s)
{
    static const uint8_t B[5] = {0, 1, 2, 4, 8};
    uint32_t b = B[clamp(p[P_E5], 0, 4)], hz, best = 1, k;
    if (b)
        return b;
    hz = shd_src_hz(s);
    for (k = 1; k <= 16; k <<= 1) {                 /* no tempo: the bars that put the loop at 80..160 BPM */
        uint32_t bpm = (uint32_t)((uint64_t)k * 240u * hz / (s->len ? s->len : 1u));
        best = k;
        if (bpm >= 80u)
            break;
    }
    return best;
}
/* BARS AUTO with a tempo: the sample's beats at its tempo, played at the song's (beat_samples) */
static uint32_t shd_loop_len(const int16_t *p, const slc_src_t *s)
{
    uint32_t b10 = clamp(p[P_E5], 0, 4) ? 0u : shd_bpm10(slc_src_of(p)), hz = shd_src_hz(s);
    if (b10 && hz) {
        uint32_t l = (uint32_t)((uint64_t)s->len * b10 * beat_samples() / (600u * (uint64_t)hz));
        if (l)
            return l;
    }
    return shd_bars(p, s) * 4u * beat_samples();
}
static uint32_t shd_at(const slc_src_t *s, uint32_t div, uint32_t f, uint32_t k, uint32_t loop)
{
    uint32_t a, b, st, a0, len = s->len ? s->len : 1u;
    slc_bounds(s, div, f, &a0, &b, &st);
    slc_bounds(s, div, k, &a, &b, &st);
    return (uint32_t)((uint64_t)((a + len - a0) % len) * loop / len);
}

/* start slice j on a voice (a note, or PLAY moving on): decoder, envelope */
static void shd_start(track_t *t, voice_t *v, const slc_src_t *s, uint32_t src, uint32_t div, uint32_t j)
{
    uint32_t a, b, st, rev;
    shd_vx_t *x = shd_vx(t, v);
    const shd_sl_t *sl = &shd_kit[src].sl[j % SLC_AUTO];
    slc_bounds(s, div, j, &a, &b, &st);
    rev = sl->rev == SHD_REV;
    v->s[4] = (int32_t)(src | (st >> 24) << 3 | rev << 7 | j << 8 | div << 16);
    v->s[6] = 0;
    x->env = 0;
    x->stage = 0;
    x->pinged = 0;
    x->idle = b <= a;
    if (rev) {
        v->ph[0] = b;
        v->ph[2] = a;
        v->s[0] = 0x7FFFFFFF;                       /* no window yet */
    } else {
        v->ph[0] = a;
        v->ph[2] = b;
        v->s[0] = (int16_t)(st & 0xFFFFu);
        v->s[1] = (int32_t)((st >> 16) & 127u);
    }
}

static void shard_note_on(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    uint32_t src = slc_src_of(p), div = shd_div(p), j, i;
    const slc_src_t *s = slc_get(src);
    shd_vx_t *x = shd_vx(t, v);
    v->ph[1] = 0;
    v->s[2] = v->s[3] = 0;
    v->s[5] = 32767;
    v->s[7] = 0;
    v->env_out = 0;
    x->lo = x->bd = x->held = 0;
    x->hcnt = 0;
    x->choked = 0;
    x->fadein = SHD_FADE;
    x->cw = 0;
    memset(shd_mem(t)->comb[(uint32_t)(v - t->v) % NVOICE], 0, sizeof shd_mem(t)->comb[0]);
    if (!s) {                                       /* no material: the sine (as SLICE) */
        v->s[4] = (int32_t)SLC_SINE;
        v->s[6] = 1;
        v->ph[0] = 0;
        return;
    }
    j = slc_note_slice(p, v->note, slc_count(s, div));
    shd_start(t, v, s, src, div, j);
    x->first = (uint8_t)j;
    x->clock = 0;
    if (clamp(p[P_E4], 0, SHD_M_PLAY) == SHD_M_PLAY) {
        uint32_t n = slc_count(s, div);
        x->loop = shd_loop_len(p, s);
        x->next = n > 1u ? shd_at(s, div, j, (j + 1u) % n, x->loop) : x->loop;
        if (!x->next || (j + 1u) % n == j)
            x->next = x->loop;
    }
    /* CHOKE: the part's other voices of the slice's group fade out */
    if (shd_kit[src].sl[j % SLC_AUTO].choke)
        for (i = 0; i < NVOICE; i++) {
            voice_t *o = &t->v[i];
            if (o != v && o->active && ((uint32_t)o->s[4] & 7u) != SLC_SINE &&
                shd_sl(o)->choke == shd_kit[src].sl[j % SLC_AUTO].choke)
                shd_vx(t, o)->choked = 1;
        }
}

/* ONE: no release; the ADSR as it is otherwise (PLAY and GATE release, LOOP repeats while held) */
static int32_t shard_amp(track_t *t, voice_t *v, int32_t adsr)
{
    if (((uint32_t)v->s[4] & 7u) != SLC_SINE && v->stage == 3u && shd_mode(t->p, shd_sl(v)) == SHD_M_ONE)
        v->stage = 2;
    return adsr;
}

/* PLAY: the loop moves on to the next slice (or round to the note's) */
static void shd_advance(track_t *t, voice_t *v, const slc_src_t *s, shd_vx_t *x)
{
    uint32_t pk = (uint32_t)v->s[4], src = pk & 7u, div = (pk >> 16) & 15u, n = slc_count(s, div);
    uint32_t k = (shd_slice_of(v) + 1u) % (n ? n : 1u), k2;
    if (k == x->first)                              /* round the loop: the clock from its start again */
        x->clock = x->clock >= x->loop ? x->clock - x->loop : 0u;
    shd_start(t, v, s, src, div, k);
    v->ph[1] = 0;
    v->s[2] = v->s[3] = 0;
    x->fadein = SHD_FADE;
    k2 = (k + 1u) % (n ? n : 1u);
    x->next = k2 == x->first ? x->loop : shd_at(s, div, x->first, k2, x->loop);
    if (x->next <= x->clock)
        x->next = x->clock + 1u;
}

/* source samples per output sample (Q16): the material's rate, PTCH and the slice's PITCH (varispeed); the key
 * picks the slice, it does not transpose (as SLICE) */
static uint32_t shd_step(const int16_t *p, const shd_sl_t *sl, const slc_src_t *s, const vmod_t *m, const voice_t *v)
{
    return (pow2_q16(clamp(m->pitch16 - v->pitch_cur + (p[P_E3] + sl->pitch) * 16, -1536, 576)) >> 8) * (s->rate >> 8);
}

/* DIR PING: the voice turns at its slice's end (to go back) or its start (forward again) */
static void shd_turn(const slc_src_t *s, voice_t *v, slc_dec_t *d, uint32_t to_rev)
{
    uint32_t a, b, st;
    slc_bounds_v(s, v, &a, &b, &st);
    if (to_rev) {
        v->s[4] = (int32_t)((uint32_t)v->s[4] | 1u << 7);
        v->ph[0] = b;
        v->ph[2] = a;
        v->s[0] = 0x7FFFFFFF;                       /* no window yet */
    } else {
        v->s[4] = (int32_t)(((uint32_t)v->s[4] & ~(1u << 7 | 15u << 3)) | (st >> 24) << 3);
        slc_dec_at(d, a, st);
        v->ph[0] = a;
        v->ph[2] = b;
    }
}

static void shard_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    shd_mem_t *mem = shd_mem(t);
    shd_vx_t *x = shd_vx(t, v);
    uint32_t pk = (uint32_t)v->s[4], i, frac = v->ph[1], stepq = 0, play, mode;
    const slc_src_t *s = v->s[6] == 2 || (pk & 7u) == SLC_SINE ? 0 : slc_get(pk & 7u);
    int16_t *rb = mem->rb[(uint32_t)(v - t->v) % NVOICE], *cb = mem->comb[(uint32_t)(v - t->v) % NVOICE];
    const shd_sl_t *sl;
    slc_dec_t d;
    int32_t xin = 0;
    if ((pk & 7u) == SLC_SINE && v->s[6] != 2) {
        smp_sine(out, n, m, p[P_E3] * 16, 32767, &v->ph[0], &v->s[7]);
        return;
    }
    if (!s) {
        v->active = 0;
        return;
    }
    play = clamp(p[P_E4], 0, SHD_M_PLAY) == SHD_M_PLAY;
    if (play) {                                     /* the tempo moved: the loop with it, the clock in proportion */
        uint32_t loop = shd_loop_len(p, s);
        if (loop != x->loop && x->loop) {
            x->clock = (uint32_t)((uint64_t)x->clock * loop / x->loop);
            x->next = (uint32_t)((uint64_t)x->next * loop / x->loop);
            x->loop = loop;
        }
    }
    d.pos = v->ph[0];
    d.pred = v->s[0];
    d.idx = v->s[1];
    d.seg = (pk >> 3) & 15u;
    sl = shd_sl(v);
    stepq = shd_step(p, sl, s, m, v);
    for (i = 0; i < n; i++) {
        int32_t y, g, f, q;
        uint32_t rev;
        if (play && ++x->clock >= x->next) {       /* PLAY: the next slice */
            if (!((uint32_t)v->s[4] >> 7 & 1u)) {
                v->ph[0] = d.pos;
                v->s[0] = d.pred;
                v->s[1] = d.idx;
            }
            shd_advance(t, v, s, x);
            d.pos = v->ph[0];
            d.pred = v->s[0];
            d.idx = v->s[1];
            d.seg = ((uint32_t)v->s[4] >> 3) & 15u;
            frac = 0;
            stepq = shd_step(p, shd_sl(v), s, m, v);
        }
        pk = (uint32_t)v->s[4];
        rev = (pk >> 7) & 1u;
        sl = shd_sl(v);
        mode = shd_mode(p, sl);
        if (x->idle) {
            if (!play) {
                v->s[6] = 2;
                break;
            }
            continue;
        }
        if (!v->s[6]) {                             /* prime the interpolator */
            if (!(rev ? slc_rev(s, v, rb, 0, &xin) : slc_fwd(s, v, &d, 0, &xin))) {
                x->idle = 1;
                continue;
            }
            v->s[3] = xin;
            v->s[6] = 1;
        }
        frac += stepq;
        while (frac >= 65536u) {
            int loop = mode == SHD_M_LOOP && v->gate, ping = sl->rev == SHD_PING;
            frac -= 65536u;
            v->s[2] = v->s[3];
            if (!(rev ? slc_rev(s, v, rb, loop && !ping, &xin) : slc_fwd(s, v, &d, loop && !ping, &xin))) {
                int ok = 0;
                if (ping && (loop || (!rev && !x->pinged))) {   /* PING: back (once), or to and fro (LOOP) */
                    rev ^= 1u;
                    x->pinged = 1;
                    shd_turn(s, v, &d, rev);
                    ok = rev ? slc_rev(s, v, rb, 0, &xin) : slc_fwd(s, v, &d, 0, &xin);
                }
                if (!ok) {
                    x->idle = 1;                    /* the slice ended */
                    v->s[3] = 0;
                    break;
                }
            }
            v->s[3] = xin;
        }
        if (x->idle)
            continue;
        y = v->s[2] + (((v->s[3] - v->s[2]) * (int32_t)(frac >> 1)) >> 15);
        if (sl->filt >= SHD_F_COMBN) {              /* a feedback comb: CUT its pitch, RES its feedback (- odd) */
            uint32_t dq = SHD_COMB_D[sl->cut & 127], di = dq >> 8, df = dq & 255u;
            uint32_t r0 = (x->cw - di) & (SHD_COMB - 1u), r1 = (r0 - 1u) & (SHD_COMB - 1u);
            int32_t dl = (cb[r0] * (int32_t)(256u - df) + cb[r1] * (int32_t)df) >> 7, gq = SHD_COMB_G[sl->res & 127];
            int32_t w = shd_sat(y + (int32_t)(((int64_t)dl * (sl->filt == SHD_F_COMBN ? -gq : gq)) >> 15), 65535);
            cb[x->cw] = (int16_t)(w >> 1);
            x->cw = (uint16_t)((x->cw + 1u) & (SHD_COMB - 1u));
            y = (int32_t)(((int64_t)w * (32768 - gq / 2)) >> 15);
        } else if (sl->filt) {                      /* the slice's filter: Chamberlin at 2x */
            int32_t ff = SHD_SVF_F[sl->cut & 127], qq = SHD_SVF_Q[sl->res & 127], in = shd_sat(y, 65535), hi = 0, k;
            for (k = 0; k < 2; k++) {
                x->lo = shd_sat(x->lo + ((ff * x->bd) >> 14), 65535);
                hi = shd_sat(in - x->lo - ((qq * x->bd) >> 14), 65535);
                x->bd = shd_sat(x->bd + ((ff * hi) >> 14), 65535);
            }
            y = sl->filt == SHD_F_LP ? x->lo : sl->filt == SHD_F_HP ? hi : x->bd;
        }
        if (sl->dtype)
            y = shd_shape(sl->dtype, y, sl->drv, &x->held, &x->hcnt);
        /* the slice's envelope: attack, a squared decay (127: none) to SUSTAIN, held; RELEASE (OFF: the track's) from
         * the key's release (not ONE's) */
        if (x->stage < 3u && sl->rel && !v->gate && mode != SHD_M_ONE)
            x->stage = 3;
        if (!x->stage) {
            uint32_t st = SHD_ENV_STEP[sl->atk & 127];
            x->env = x->env + st >= (1u << 30) ? 1u << 30 : x->env + st;
            if (x->env == 1u << 30)
                x->stage = 1;
            g = (int32_t)(x->env >> 15);
        } else {
            uint32_t e;
            if (x->stage == 1u && sl->dec < 127u) {
                uint32_t st = SHD_ENV_STEP[sl->dec & 127], fl = SHD_SUS_ENV[sl->sus & 127];
                x->env = x->env > fl + st ? x->env - st : fl;
                if (x->env == fl) {
                    if (!fl) {
                        x->idle = 1;
                        continue;
                    }
                    x->stage = 2;
                }
            } else if (x->stage == 3u) {
                uint32_t st = SHD_ENV_STEP[sl->rel & 127];
                x->env = x->env > st ? x->env - st : 0;
                if (!x->env) {
                    x->idle = 1;
                    continue;
                }
            }
            e = x->env >> 15;
            g = (int32_t)((e * e) >> 15);
        }
        g = (int32_t)(((int64_t)g * ((int32_t)(sl->level & 127) * 327)) >> 15);   /* LEVEL: 100 = 1.0 */
        if (play) {                                 /* fades at the slice changes */
            uint32_t left = x->next - x->clock;
            if (x->fadein) {
                g = g * (int32_t)(SHD_FADE - x->fadein) / (int32_t)SHD_FADE;
                x->fadein--;
            }
            if (left < SHD_FADE)
                g = g * (int32_t)left / (int32_t)SHD_FADE;
        } else {                                    /* the last ~1.5 ms of a one-shot */
            uint32_t rem = rev ? v->ph[0] - v->ph[2] : v->ph[2] - d.pos;
            if (!(mode == SHD_M_LOOP && v->gate) && !(sl->rev == SHD_PING && !rev && !x->pinged) &&
                rem < ((stepq * 64u) >> 16) + 1u) {
                uint32_t left = (rem << 16) / (stepq ? stepq : 1u);
                if (left < 64u)
                    g = g * (int32_t)left >> 6;
            }
        }
        if (x->choked)                              /* CHOKE: out over this block */
            g = g * (int32_t)(n - i) / (int32_t)n;
        f = (int32_t)(((int64_t)y * g) >> 15);
        q = voice_amp(f, m, i) << 1;
        if (sl->dly) {                              /* DELAY: the slice's send (fx.c shard_send) */
            mem->dsend[i] += (int32_t)(((int64_t)q * ((int32_t)(sl->dly & 127) * 258)) >> 15);
            mem->dused = 1;
        }
        if (sl->route == SHD_DIRECT || sl->route >= SHD_NROUTE)
            out[i] += q;
        else {
            mem->bus[sl->route - 1u][i] += q;
            mem->used |= 1u << (sl->route - 1u);
        }
    }
    v->ph[1] = frac;
    if (!((uint32_t)v->s[4] >> 7 & 1u)) {
        v->ph[0] = d.pos;
        v->s[0] = d.pred;
        v->s[1] = d.idx;
        v->s[4] = (int32_t)(((uint32_t)v->s[4] & ~(15u << 3)) | d.seg << 3);
    }
    if (x->choked || (x->idle && !play) || v->s[6] == 2)
        v->active = 0;
}

/* once a block, before the voices: the buses empty */
static void shard_block(track_t *t)
{
    shd_mem_t *mem = shd_mem(t);
    uint32_t b, i;
    if (mem->used)
        for (b = 0; b <= SHD_NCH; b++)
            for (i = 0; i < CTL; i++)
                mem->bus[b][i] = 0;
    if (mem->dused)
        for (i = 0; i < CTL; i++)
            mem->dsend[i] = 0;
    mem->used = mem->dused = 0;
}

/* after the voices: the multiband distortion over the buses, into the part */
static uint32_t shard_post(track_t *t, int32_t *out, uint32_t n)
{
    shd_mem_t *mem = shd_mem(t);
    const shd_bus_t *bs = &shd_kit[slc_src_of(t->p)].bus;
    int32_t al = SHD_ONEPOLE[bs->lo & 127], ah = SHD_ONEPOLE[bs->hi & 127], mix = clamp(t->p[P_E6], 0, 127);
    int32_t bias = clamp(t->p[P_E7], -64, 63);
    uint32_t i, b, dr[SHD_NCH];
    if (mem->used)
        mem->tail = 64u;                            /* ~46 ms of shaper and gate tails */
    else if (!mem->tail)
        return 0;
    else
        mem->tail--;
    if (ah < al) {
        int32_t tmp = al;
        al = ah;
        ah = tmp;
    }
    for (b = 0; b < SHD_NCH; b++)
        dr[b] = (uint32_t)clamp((int32_t)bs->drive[b] + bias, 0, 127);
    for (i = 0; i < n; i++) {
        int32_t xx = shd_sat(mem->bus[0][i], 1 << 17), split[3], wet = 0, dc;
        mem->lpl += (int32_t)(((int64_t)(xx - mem->lpl) * al) >> 15);
        mem->lph += (int32_t)(((int64_t)(xx - mem->lph) * ah) >> 15);
        split[0] = mem->lpl;
        split[1] = mem->lph - mem->lpl;
        split[2] = xx - mem->lph;
        for (b = 0; b < SHD_NCH; b++) {             /* LOW MID HIGH: the split + their own; FULL: its own */
            int32_t in = shd_sat((b < 3u ? split[b] : 0) + mem->bus[b + 1u][i], 1 << 17), a = in < 0 ? -in : in, tg, y;
            mem->fol[b] += a > mem->fol[b] ? (a - mem->fol[b]) >> 4 : -(mem->fol[b] >> 11);
            tg = !bs->gate[b] || mem->fol[b] > SHD_GATE_Q15[bs->gate[b] & 127] ? 32768 : 0;
            mem->gain[b] += (tg - mem->gain[b]) >> 7;
            y = shd_shape(bs->type[b], in, dr[b], &mem->held[b], &mem->cnt[b]);
            y = (int32_t)(((int64_t)y * mem->gain[b]) >> 15);
            wet += (int32_t)(((int64_t)y * ((int32_t)(bs->level[b] & 127) * 258)) >> 15);
        }
        dc = wet - mem->dcx + (int32_t)(((int64_t)mem->dcy * 32700) >> 15);   /* DC blocker, ~10 Hz */
        mem->dcx = wet;
        mem->dcy = dc;
        out[i] += (xx * (127 - mix) + shd_sat(dc, 1 << 19) * mix) / 127;
    }
    return 1;
}

/* three factory sounds on BREAK; the kit (per-slice settings) is the source's, edited on SLICE FX */
static const preset_t SHARD_PRESETS[] = {
    {"RECYCLE", {0, SHD_DIV_AUTO, 0, 0, SHD_M_PLAY, 0, 127, 0}, {0, 127, 127, 20}, 0, 0, FX(0, 0, 0, 10), PAT(11)},
    {"CHOP KIT", {0, SHD_DIV_AUTO, 0, 0, SHD_M_ONE, 0, 127, 0}, {0, 127, 127, 30}, 0, 0, FX(0, 0, 0, 12), PAT(9)},
    {"SHRED", {0, 5, 0, 0, SHD_M_GATE, 0, 127, 24}, {0, 127, 127, 12}, 0, 0, FX(0, 0, 20, 10), PAT(10)},
};

static const engine_t ENG_SHARD = {
    .name = "SHARD",
    .page_title = {"SHRD", "PLAY"},
    .edit = {
        {"SRC", F_ENUM, 0, SLC_NSRC - 1, 0, N_SLC_SRC, 0},
        {"DIV", F_ENUM, 0, SHD_NDIV - 1, SHD_DIV_AUTO, N_SHD_DIV, 0},
        {"START", F_INT, 0, 31, 0, 0, 0},
        {"PTCH", F_SEMI, -24, 24, 0, 0, 0},
        {"MODE", F_ENUM, 0, 3, SHD_M_PLAY, N_SHD_MODE, 0},
        {"BARS", F_ENUM, 0, 4, 0, N_SHD_BARS, 0},
        {"MULTI", F_INT, 0, 127, 127, 0, 0},
        {"DRIVE", F_INT, -64, 63, 0, 0, 0},
    },
    .presets = SHARD_PRESETS,
    .npresets = NELEM(SHARD_PRESETS),
    .note_on = shard_note_on,
    .render = shard_render,
    .knob = {P_E0, P_E1, P_E4, P_E7},
    .sampled = 1,
    .keys = slice_keys,
    .amp = shard_amp,
    .block = shard_block,
    .post = shard_post,
    .mem = sizeof(shd_mem_t),
};

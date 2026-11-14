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
/* GRAIN (the slice's, or the track's on SLICE FX's BUS): the slice played as grains, its pitch apart from its time */
enum { SHD_GM_OFF, SHD_GM_STRETCH, SHD_GM_CLOUD, SHD_GM_FREEZE, SHD_NGM };
#define SHD_GT_TRACK 0u                             /* a slice's GRAIN: 0 the track's, else SHD_GM_* + 1 */
#define SHD_GSIZE_DEF 77u                           /* SIZE 0..127: 4 .. 256 ms (77: 50 ms) */
#define SHD_GDENS_DEF 42u                           /* DENS 0..127: grains overlapping, 1 .. 4 (42: 2) */

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
#if FELUCCA_MANGLER
#define SHD_STRETCH "STRETCH"
#else
#define SHD_STRETCH "STRTCH"                        /* (Felucca's card font: 6 letters fit) */
#endif
static const char *const N_SHD_GRAIN[] = {"OFF", SHD_STRETCH, "CLOUD", "FREEZE"};
static const char *const N_SHD_SGRAIN[] = {"TRACK", "OFF", SHD_STRETCH, "CLOUD", "FREEZE"};

/* ---- the kit: per source, per slice (SLC_AUTO = 32 at most), and the bus */
typedef struct {
    int8_t pitch;
    uint8_t level, atk, dec, mode, rev, filt, cut, res, dtype, drv, route, choke;
    uint8_t sus, rel, dly;                          /* SUSTAIN, RELEASE (0: OFF, the track's), DELAY's send */
    uint8_t grain, gsize, gdens, gjit;              /* GRAIN (SHD_GT_TRACK, else SHD_GM_* + 1), SIZE, DENS, JITTER */
} shd_sl_t;
typedef struct {
    uint8_t lo, hi;                                 /* split points: 40 Hz .. 12 kHz */
    uint8_t type[SHD_NCH], drive[SHD_NCH], gate[SHD_NCH], level[SHD_NCH];   /* low, mid, high, full */
    uint8_t bpm;                                    /* the source's tempo (40..250), 0: AUTO (shd_tempo_scan) */
    uint8_t sens;                                   /* AUTO's hits: 0 fewest .. 127 most (SHD_SENS_DEF: SLICE's) */
    uint8_t gmode, gsize, gdens, gjit;              /* the track's GRAIN (SHD_GM_*), SIZE, DENS, JITTER */
    uint8_t fb[SHD_NCH];                            /* FDBK: each channel's shaper fed back into itself (0 none) */
} shd_bus_t;
typedef struct {
    shd_sl_t sl[SLC_AUTO];
    shd_bus_t bus;
} shd_kit_t;
_Static_assert(sizeof(shd_sl_t) == 20u && sizeof(shd_bus_t) == 28u, "SHARD kit layout");
/* neutral at power-on (the slot scans then read the stored kits): every slice as recorded, DIRECT */
#define SHD_SL_DEF {0, 100, 0, 127, SHD_TRACK, SHD_FWD, SHD_F_OFF, 127, 0, SHD_D_OFF, 40, SHD_DIRECT, 0, 0, 0, 0, \
                    SHD_GT_TRACK, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0}
#define SHD_BUS_DEF {51, 96, {SHD_D_TUBE, SHD_D_FOLD, SHD_D_CRUSH, SHD_D_OFF}, {40, 30, 60, 40}, {0, 0, 0, 0}, \
                     {100, 100, 90, 127}, 0, SHD_SENS_DEF, SHD_GM_OFF, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0, {0, 0, 0, 0}}
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
        if (s->grain > SHD_NGM) s->grain = SHD_GT_TRACK;
        if (s->gsize > 127) s->gsize = SHD_GSIZE_DEF;
        if (s->gdens > 127) s->gdens = SHD_GDENS_DEF;
        if (s->gjit > 127) s->gjit = 0;
    }
    if (k->bus.lo > 127) k->bus.lo = db.lo;
    if (k->bus.hi > 127) k->bus.hi = db.hi;
    for (b = 0; b < SHD_NCH; b++) {
        if (k->bus.type[b] >= SHD_NDRIVE) k->bus.type[b] = db.type[b];
        if (k->bus.drive[b] > 127) k->bus.drive[b] = db.drive[b];
        if (k->bus.gate[b] > 127) k->bus.gate[b] = 0;
        if (k->bus.level[b] > 127) k->bus.level[b] = db.level[b];
        if (k->bus.fb[b] > 127) k->bus.fb[b] = 0;
    }
    if (k->bus.bpm && (k->bus.bpm < SHD_BPM_MIN || k->bus.bpm > SHD_BPM_MAX)) k->bus.bpm = 0;
    if (k->bus.sens > 127) k->bus.sens = SHD_SENS_DEF;
    if (k->bus.gmode >= SHD_NGM) k->bus.gmode = SHD_GM_OFF;
    if (k->bus.gsize > 127) k->bus.gsize = SHD_GSIZE_DEF;
    if (k->bus.gdens > 127) k->bus.gdens = SHD_GDENS_DEF;
    if (k->bus.gjit > 127) k->bus.gjit = 0;
}

/* ---- a shaper's state (shd_shape): DECIM's held sample, and the analog models' filters and bias */
typedef struct {
    int32_t held, prev;                             /* DECIM's sample (cnt its count); the last pre-clip value (2x) */
    int32_t f[6];                                   /* one-pole states: coupling, legs, tone stacks, DC */
    int32_t env;                                    /* OVRDRV: the grid's bias */
    uint16_t cnt;
} shd_ds_t;

/* ---- GRAIN: a voice's grains, and the head that walks its slice (shd_gr_*) */
#define SHD_GN 4u                                   /* grains per voice (DENS: 4 overlap at most) */
#define SHD_GCP 16u                                 /* the head's decoder states kept: every SHD_GCPS source samples */
#define SHD_GCPS 128u
#define SHD_GPART 8u                                /* grains sounding per part at most (the CPU: GRAIN's worst alike) */
#define SHD_GBUD 384u                               /* samples decoded to seek new grains: per block, per part, */
#define SHD_GBUD_MAX 2048u                          /* saved up to this (a grain now and then: aligned) */
typedef struct {
    uint32_t pos;                                   /* its decoder (as slc_dec_t, packed): the next sample to read */
    int16_t pred;
    uint8_t idx, seg;
    uint32_t frac, rate;                            /* Q16: between a and b; source samples per output sample */
    uint32_t wph, winc;                             /* the window's phase (2^32 over the grain); winc 0 = free */
    int16_t a, b;
} shd_gr_t;
typedef struct {
    shd_gr_t g[SHD_GN];
    slc_dec_t hd;                                   /* the head's decoder (forward: the states it leaves in cp) */
    uint32_t head, hfrac;                           /* the head: source sample, Q16 fraction */
    uint32_t cp[SHD_GCP];                           /* decoder states at multiples of SHD_GCPS: cplo .. hd.pos */
    uint32_t cplo;
    uint32_t anc, ancst;                            /* the slice's start and its state */
    uint32_t wait, hold;                            /* output samples to the next grain; FREEZE (ONE): held for */
    uint8_t mode, rev, run, held;                   /* SHD_GM_* (0: not grains); the head goes back; makes grains */
    uint8_t first;                                  /* the slice's first grains still to make (shd_gr_setup) */
} shd_gv_t;

/* FDBK: each channel's loop band-pass, Chamberlin f = 2 sin(pi fc / fs) Q14: LOW 150 Hz, MID 900 Hz, HIGH 4 kHz,
 * FULL 500 Hz; the gain FDBK^2 x 4 (Q15: 64 = 0.5, 127 = 2.0), over the shaper's gain as the followers find it
 * (in / out, up to 4: its drive out of the loop), times the band-pass's 2: from about 64 the loop sustains itself,
 * whatever the type and drive, held at the shaper's ceiling */
static const int32_t SHD_FB_F[SHD_NCH] = {350, 2100, 9226, 1167};
static volatile uint32_t shd_swell;                 /* perform.c (ARP's FEEDBACK SWELL): every FDBK at least this */

/* ---- the part's engine memory */
typedef struct {
    int32_t lo, bd;                                 /* the filter */
    shd_ds_t ds;                                    /* the slice's shaper */
    uint32_t env;                                   /* the slice's envelope, 0 .. 1 << 30 */
    uint32_t clock, next, loop;                     /* PLAY: samples since the note, the next slice's start, the loop */
    uint16_t fadein, cw;                            /* (cw: the comb line's write point) */
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
    int32_t fol[SHD_NCH], gain[SHD_NCH];
    shd_ds_t ds[SHD_NCH];                           /* the channels' shapers */
    int32_t fbl[SHD_NCH], fbb[SHD_NCH], fby[SHD_NCH];   /* FDBK: the loop's band-pass (low, band), the last out, */
    int32_t fai[SHD_NCH], fao[SHD_NCH];             /* .. the shaper's mean in and out (its gain: their ratio) */
    shd_gv_t gv[NVOICE];                            /* GRAIN: each voice's grains */
    uint32_t gload, gbud;                           /* GRAIN: grains sounding (counted each block), seek budget */
    int32_t grng;
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
#define SHD_LP(st, in, a) ((st) += (int32_t)(((int64_t)((in) - (st)) * (a)) >> 15))   /* a one-pole low-pass */
/* a pair of silicon diodes: linear to 0.5, a quadratic knee to 1 at 1.5, flat beyond (Q15) */
static int32_t shd_diode(int32_t x)
{
    int32_t a = x < 0 ? -x : x, y, d;
    if (a <= 16384)
        return x;
    if (a >= 49152)
        y = 32768;
    else {
        d = a - 16384;
        y = 16384 + d - (int32_t)(((int64_t)d * d) >> 16);
    }
    return x < 0 ? -y : y;
}
/* a transistor stage's soft clip without a division: 1.5 x - 0.5 x^3, flat past 1 (Q15) */
static int32_t shd_cubic(int32_t x)
{
    int32_t x3;
    if (x >= 32768)
        return 32767;
    if (x <= -32768)
        return -32767;
    x3 = (int32_t)((((int64_t)x * x >> 15) * x) >> 15);
    return (3 * x - x3) >> 1;
}
/* a triode's curve: grid conduction rounds the top early, cutoff clips the bottom harder and later (to 1.5) */
static int32_t shd_triode(int32_t p)
{
    return p >= 0 ? shd_soft(p) : -((shd_diode(-p - (-p >> 2)) * 3) >> 1);   /* (x 3/4 in, x 3/2 out: slope 1 at 0) */
}

/* OVRDRV: a 12AX7 gain stage. The coupling capacitor (80 Hz) in; x1 .. x48; the grid: peaks that drive it positive
 * charge the coupling capacitor, biasing the stage colder for a moment (the blocking sag after a hit, dynamic
 * asymmetry); the triode's curve at 2x; the plate's DC out (20 Hz); the Miller capacitance's roll-off, 9 kHz at
 * low gain to 4.5 kHz at full */
static int32_t shd_tube(int32_t x, uint32_t d, shd_ds_t *st)
{
    int32_t u, m, y;
    SHD_LP(st->f[0], x, SHD_A80);
    u = shd_sat((int32_t)(((int64_t)(x - st->f[0]) * SHD_TUBE_G[d]) >> 8), 1 << 23);
    if (u > st->env)
        st->env += (u - st->env) >> 5;              /* ~0.7 ms to charge */
    else
        st->env -= st->env >> 11;                   /* ~46 ms to let go */
    u -= st->env >> 2;
    m = (st->prev + u) >> 1;                        /* 2x: the midpoint, then the sample, averaged */
    st->prev = u;
    y = (shd_triode(m) + shd_triode(u)) >> 1;
    SHD_LP(st->f[1], y, SHD_A20);
    y -= st->f[1];
    SHD_LP(st->f[2], y, SHD_TUBE_LP[d]);
    return (st->f[2] * 17000) >> 15;              /* (level: about the clean signal's at low drive) */
}

/* DIST: a ProCo RAT. The LM308's gain is 1 + R / Z with DISTORTION's R (0 .. 100 k) over two legs to ground,
 * 47 R + 2.2 uF (above 1.5 kHz) and 560 R + 4.7 uF (above 60 Hz): the highs gain up to x2100, the lows x180,
 * so the bass stays tight; its gain-bandwidth closes the top as the gain rises (20 kHz .. 2.5 kHz); the rails
 * round it at ~7.5 diode drops; the 1N914 pair to ground clips it hard (2x); the FILTER after, ~4.5 kHz */
static int32_t shd_rat(int32_t x, uint32_t d, shd_ds_t *st)
{
    int32_t u, m, y;
    int64_t g;
    SHD_LP(st->f[0], x, SHD_A60);
    SHD_LP(st->f[1], x, SHD_A1539);
    g = (int64_t)(x - st->f[1]) * SHD_RAT_HI[d] + (int64_t)(x - st->f[0]) * SHD_RAT_LO[d];
    g = x + (g >> 8);
    u = (int32_t)(g > (1 << 23) ? 1 << 23 : g < -(1 << 23) ? -(1 << 23) : g);
    SHD_LP(st->f[2], u, SHD_RAT_LP[d]);
    u = (shd_soft(st->f[2] * 2 / 15) * 15) >> 1;   /* the rails: 7.5 x tanh(u / 7.5) */
    m = (st->prev + u) >> 1;
    st->prev = u;
    y = (shd_diode(m) + shd_diode(u)) >> 1;
    SHD_LP(st->f[3], y, SHD_A4500);
    return (st->f[3] * 18000) >> 15;
}

/* FUZZ: after Death By Audio's Fuzz War, a Supa Tone Bender (Big Muff) pushed further: three transistor stages of
 * x1 .. x14 each (x2700 in all), with the Muff's coupling and roll-off between them; a starved first stage's sputter:
 * input under a threshold that rises with the gain (to ~5 % of full scale) is dropped, (notes break up and gate as they decay); the
 * Muff's tone stack at noon (400 Hz low-pass and 1.1 kHz high-pass, mixed: the scooped mids); a clipping stage
 * (diodes, x3, 2x) and the output. At DRIVE 0 it only shapes the tone, as the pedal at its lowest */
static int32_t shd_war(int32_t x, uint32_t d, shd_ds_t *st)
{
    int32_t g = SHD_WAR_G[d], th = SHD_WAR_TH[d], a, m, y;
    SHD_LP(st->f[0], x, SHD_A120);
    a = x - st->f[0];
    a = a > th ? a - th : a < -th ? a + th : 0;    /* the sputter: what can't turn the first stage on is lost */
    a = shd_cubic(shd_sat((int32_t)(((int64_t)a * g) >> 8), 1 << 22));
    SHD_LP(st->f[1], a, SHD_A200);
    a -= st->f[1];
    SHD_LP(st->f[2], a, SHD_A5500);
    a = shd_cubic((st->f[2] * g) >> 8);
    a = shd_cubic((a * g) >> 8);
    SHD_LP(st->f[3], a, SHD_A400);
    SHD_LP(st->f[4], a, SHD_A1100);
    a = (st->f[3] + a - st->f[4]) * 3 >> 1;         /* the tone stack (1/2 each), then x3 into the diodes */
    m = (st->prev + a) >> 1;
    st->prev = a;
    y = (shd_diode(m) + shd_diode(a)) >> 1;
    SHD_LP(st->f[5], y, SHD_A20);                   /* the output capacitor: no DC (the stages' x2700 finds any) */
    return ((y - st->f[5]) * 19000) >> 15;
}

static int32_t shd_shape(uint32_t type, int32_t x, uint32_t drive, shd_ds_t *st)
{
    int32_t p, y;
    drive = drive > 127u ? 127u : drive;
    switch (type) {                                 /* the analog models */
    case SHD_D_SOFT: return shd_tube(shd_sat(x, 1 << 17), drive, st);
    case SHD_D_HARD: return shd_rat(shd_sat(x, 1 << 17), drive, st);
    case SHD_D_FUZZ: return shd_war(shd_sat(x, 1 << 17), drive, st);
    default: break;
    }
    p = (int32_t)(((int64_t)shd_sat(x, 1 << 17) * SHD_DRIVE_Q8[drive]) >> 8);
    switch (type) {
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
        if (++st->cnt >= 1u + drive * 31u / 127u) {
            st->cnt = 0;
            st->held = x;
        }
        return st->held;
    case SHD_D_HALF: y = p > 0 ? shd_soft(p) : shd_soft(p) >> 2; break;
    case SHD_D_TUBE: y = p >= 0 ? shd_soft(p) : (shd_soft(p + (p >> 1)) * 3) >> 2; break;
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

static shd_gv_t *shd_gv(track_t *t, voice_t *v);
static void shd_gr_start(shd_gv_t *gv, uint32_t src, const shd_sl_t *sl, uint32_t a, uint32_t b, uint32_t st, uint32_t rev);
/* start slice j on a voice (a note, or PLAY moving on): decoder, envelope, its grains (GRAIN) */
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
    shd_gr_start(shd_gv(t, v), src, sl, a, b, st, rev);
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
    x->lo = x->bd = 0;
    memset(&x->ds, 0, sizeof x->ds);
    x->choked = 0;
    x->fadein = SHD_FADE;
    x->cw = 0;
    memset(shd_mem(t)->comb[(uint32_t)(v - t->v) % NVOICE], 0, sizeof shd_mem(t)->comb[0]);
    memset(shd_gv(t, v), 0, sizeof(shd_gv_t));       /* (no grains: the sine has none) */
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

/* LOOP (ui_slice.c): where the part's voices are (source samples) and the slice each note began on; max at most.
 * The main loop reads words the audio writes: a position a block old at worst */
static uint32_t shd_heads(track_t *t, uint32_t *pos, uint8_t *first, uint32_t max)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE && n < max; i++) {
        voice_t *v = &t->v[i];
        shd_vx_t *x = shd_vx(t, v);
        if (!v->active || ((uint32_t)v->s[4] & 7u) == SLC_SINE)   /* (PLAY waiting for its next slice: still there) */
            continue;
        pos[n] = (uint32_t)v->ph[0];
        first[n++] = x->first;
    }
    return n;
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

/* ---- GRAIN: the slice as grains, its pitch apart from its time. A head walks the slice at the time rate (the
 * material's own; PLAY: what fills the slice's steps at the song's tempo); every hop a grain (a Hann window, SIZE long,
 * DENS overlapping) starts at the head (STRETCH, FREEZE: up to JITTER grains behind it; CLOUD: anywhere around it as
 * far as JITTER, at random hops, detuned at random by up to a semitone) and reads at the pitch rate (PTCH, the slice's
 * PITCH, the modulation: the length stays). The slice's first grains begin on its first sample at their peak: its
 * attack whole. No grain reads past the slice's end (SIZE shortens for a short slice). FREEZE: the head stops where the
 * last grain fits and the grains go on there (held: while the key is; ONE: as long again as the slice; PLAY: to the
 * next slice). REV walks the head back (the grains still read forwards); PING turns it. Seeking a grain's start
 * decodes from the nearest state kept (the head's, every SHD_GCPS behind it, the slice's start, SLICE's grid), within
 * SHD_GBUD a block per part (saved up to SHD_GBUD_MAX); at most SHD_GPART grains sound per part (a grain past either
 * is left out, or not aligned). */
typedef struct {
    uint32_t prate, trate;                          /* Q16 source samples per output sample: the grains, the head */
    uint32_t len, winc, hop;                        /* a grain: output samples, its window's step; between grains */
    uint32_t span, scat, a, b;                      /* source samples: a grain's, the scatter; the slice */
    int32_t gain;                                   /* Q15: the overlap's level */
    uint32_t jit;
} shd_gs_t;
static shd_gv_t *shd_gv(track_t *t, voice_t *v) { return &shd_mem(t)->gv[(uint32_t)(v - t->v) % NVOICE]; }

/* the GRAIN a slice plays (SHD_GM_*): its own, or the track's (SLICE FX's BUS); g: SIZE DENS JITTER */
static uint32_t shd_gmode(uint32_t src, const shd_sl_t *sl, uint8_t *g)
{
    const shd_bus_t *b = &shd_kit[src < SHD_NSRC ? src : 0u].bus;
    if (sl->grain == SHD_GT_TRACK) {
        g[0] = b->gsize;
        g[1] = b->gdens;
        g[2] = b->gjit;
        return b->gmode < SHD_NGM ? b->gmode : SHD_GM_OFF;
    }
    g[0] = sl->gsize;
    g[1] = sl->gdens;
    g[2] = sl->gjit;
    return sl->grain <= SHD_NGM ? sl->grain - 1u : SHD_GM_OFF;
}
/* SIZE (0..127) in output samples: 4 ms x 2^(6 v / 127) */
static uint32_t shd_glen(uint32_t v) { return (uint32_t)((176ull * pow2_q16((int32_t)(clamp(v, 0, 127) * 1152 / 127))) >> 16); }
/* DENS (0..127): grains overlapping, Q8: 1 .. 4 (42: 2 exactly, the windows summing flat) */
static uint32_t shd_gov(uint32_t v)
{
    uint32_t ov = 256u + (uint32_t)clamp(v, 0, 127) * 768u / 126u;
    return ov > 1024u ? 1024u : ov;
}

/* a slice starts (shd_start): its GRAIN, the head at its start (REV: its end), no grains yet */
static void shd_gr_start(shd_gv_t *gv, uint32_t src, const shd_sl_t *sl, uint32_t a, uint32_t b, uint32_t st, uint32_t rev)
{
    uint8_t g[3];
    uint32_t i;
    gv->mode = (uint8_t)shd_gmode(src, sl, g);
    for (i = 0; i < SHD_GN; i++)
        gv->g[i].winc = 0;
    gv->anc = a;
    gv->ancst = st;
    slc_dec_at(&gv->hd, a, st);
    gv->cplo = (a + SHD_GCPS - 1u) & ~(SHD_GCPS - 1u);
    gv->head = rev ? b : a;
    gv->hfrac = 0;
    gv->rev = (uint8_t)rev;
    gv->run = 1;
    gv->held = 0;
    gv->hold = 0;
    gv->first = 1;
    gv->wait = 0;
}
/* the head back to the slice's start, going forwards (LOOP, PING) */
static void shd_gr_rewind(shd_gv_t *gv)
{
    gv->head = gv->anc;
    gv->hfrac = 0;
    gv->rev = 0;
    slc_dec_at(&gv->hd, gv->anc, gv->ancst);
    gv->cplo = (gv->anc + SHD_GCPS - 1u) & ~(SHD_GCPS - 1u);
}

/* once a block (and at PLAY's next slice): the rates and the grains' shape now */
static void shd_gr_setup(track_t *t, voice_t *v, const slc_src_t *s, const shd_vx_t *x, const vmod_t *m, shd_gs_t *gs,
                         int play)
{
    shd_gv_t *gv = shd_gv(t, v);
    const shd_sl_t *sl = shd_sl(v);
    uint32_t st, l, lim, ov, prate;
    uint8_t g[3];
    slc_bounds_v(s, v, &gs->a, &gs->b, &st);
    if (gs->b <= gs->a)
        gs->b = gs->a + 1u;
    (void)shd_gmode((uint32_t)v->s[4] & 7u, sl, g);     /* (SIZE DENS JITTER as they are now; the mode as it began) */
    prate = shd_step(t->p, sl, s, m, v);
    prate = prate < 64u ? 64u : prate > 2u << 16 ? 2u << 16 : prate;
    gs->prate = prate;
    gs->trate = s->rate;
    if (play && gv->mode != SHD_GM_FREEZE) {         /* PLAY: the rest of the slice in the rest of its steps */
        uint32_t rem = gv->rev ? (gv->head > gs->a ? gv->head - gs->a : 0u) : (gs->b > gv->head ? gs->b - gv->head : 0u);
        uint32_t out = x->next > x->clock ? x->next - x->clock : 1u;
        uint64_t r = ((uint64_t)rem << 16) / out;
        gs->trate = r > 4u << 16 ? 4u << 16 : (uint32_t)r;
    }
    l = shd_glen(g[0]);
    lim = (uint32_t)(((uint64_t)(gs->b - gs->a) << 16) / prate);
    l = l > lim ? lim : l;
    l = l < 64u ? 64u : l;
    gs->len = l;
    gs->winc = 0xFFFFFFFFu / l;
    ov = shd_gov(g[1]);
    gs->hop = l * 256u / ov;
    gs->hop = gs->hop < 16u ? 16u : gs->hop;
    gs->gain = ov <= 512u ? 32768 : (int32_t)(32768u * 512u / ov);
    gs->span = (uint32_t)(((uint64_t)l * prate) >> 16);
    gs->span = gs->span > gs->b - gs->a ? gs->b - gs->a : gs->span;
    gs->jit = g[2];
    gs->scat = gv->mode == SHD_GM_CLOUD ? (gs->b - gs->a) * gs->jit / 254u : gs->span * gs->jit / 127u;
}

/* the nearest decoder state at or before pos: SLICE's grid, the slice's start, one the head kept, the head's, hint's
 * (0: none) */
static void shd_gr_seek(const shd_gv_t *gv, const slc_src_t *s, uint32_t pos, const slc_dec_t *hint, slc_dec_t *d)
{
    uint32_t k = (uint32_t)((uint64_t)pos * SLC_GRID / s->len), bp;
    k = k < SLC_GRID ? k : SLC_GRID - 1u;
    while (k && slc_gpos(s, k) > pos)
        k--;
    while (k + 1u < SLC_GRID && slc_gpos(s, k + 1u) <= pos)
        k++;
    slc_dec_at(d, slc_gpos(s, k), s->grid[k]);
    if (gv->anc <= pos && gv->anc > d->pos)
        slc_dec_at(d, gv->anc, gv->ancst);
    bp = pos & ~(SHD_GCPS - 1u);
    if (bp >= gv->cplo && bp < gv->hd.pos && bp + SHD_GCP * SHD_GCPS > gv->hd.pos && bp > d->pos)
        slc_dec_at(d, bp, gv->cp[(bp / SHD_GCPS) % SHD_GCP]);
    if (gv->hd.pos <= pos && gv->hd.pos > d->pos)
        *d = gv->hd;
    if (hint && hint->pos <= pos && hint->pos > d->pos)
        *d = *hint;
}

/* a grain from source sample pos, age output samples into its window, at rate (hint: a state to seek from, or 0) */
static void shd_gr_spawn(shd_mem_t *mem, shd_gv_t *gv, const slc_src_t *s, const shd_gs_t *gs, uint32_t pos, uint32_t age,
                         uint32_t rate, const slc_dec_t *hint)
{
    shd_gr_t *g = 0;
    slc_dec_t d;
    uint32_t i, cost;
    for (i = 0; i < SHD_GN && !g; i++)
        if (!gv->g[i].winc)
            g = &gv->g[i];
    if (!g || mem->gload >= SHD_GPART || pos + 2u >= s->len || age >= gs->len)
        return;
    shd_gr_seek(gv, s, pos, hint, &d);
    cost = pos - d.pos;
    if (cost > mem->gbud)
        return;
    mem->gbud -= cost;
    while (d.pos < pos)
        slc_dec_next(s, &d);
    g->a = (int16_t)slc_dec_next(s, &d);
    g->b = (int16_t)slc_dec_next(s, &d);
    g->pos = d.pos;
    g->pred = (int16_t)d.pred;
    g->idx = (uint8_t)d.idx;
    g->seg = (uint8_t)d.seg;
    g->frac = 0;
    g->rate = rate;
    g->winc = gs->winc;
    g->wph = age * gs->winc;
    mem->gload++;
}

/* STRETCH, FREEZE: a new grain's start moved (within SHD_WD of pos, in lo .. hi) to where the material runs on as the
 * loudest grain sounding does (WSOLA: the best match over SHD_WW source samples, every 2nd; a coarse pass then +-1):
 * the grains add in phase, a held tone keeps its pitch. Within the part's budget, else pos as it was; *hint: the state
 * at the search's start (to seek the grain from), pos UINT32_MAX for none */
#define SHD_WD 64u
#define SHD_WW 64u
static uint32_t shd_gr_align(shd_mem_t *mem, const shd_gv_t *gv, const slc_src_t *s, uint32_t pos, uint32_t lo,
                             uint32_t hi, slc_dec_t *hint)
{
    int8_t r[SHD_WW], c[2u * SHD_WD + SHD_WW];
    const shd_gr_t *ref = 0;
    slc_dec_t d;
    int32_t wmax = 8192;
    uint32_t i, j, a, b, n, cost, best = pos, o;
    hint->pos = UINT32_MAX;
    for (i = 0; i < SHD_GN; i++)                    /* the loudest grain now */
        if (gv->g[i].winc && gr_win(gv->g[i].wph) > wmax) {
            wmax = gr_win(gv->g[i].wph);
            ref = &gv->g[i];
        }
    a = pos > lo + SHD_WD ? pos - SHD_WD : lo;
    b = pos + SHD_WD < hi ? pos + SHD_WD : hi;
    if (!ref || b <= a || b + SHD_WW >= s->len)
        return pos;
    n = b - a + SHD_WW;
    shd_gr_seek(gv, s, a, 0, &d);
    cost = (a - d.pos) + n + SHD_WW + (b - a) * SHD_WW / 32u;   /* (decodes; the products / 16) */
    if (cost + 64u > mem->gbud)
        return pos;
    mem->gbud -= cost;
    {                                               /* the reference: the grain's next samples */
        slc_dec_t rd;
        rd.pos = ref->pos;
        rd.pred = ref->pred;
        rd.idx = ref->idx;
        rd.seg = ref->seg;
        r[0] = (int8_t)(ref->a >> 8);
        r[1] = (int8_t)(ref->b >> 8);
        for (i = 2; i < SHD_WW; i++)
            r[i] = (int8_t)(rd.pos < s->len ? slc_dec_next(s, &rd) >> 8 : 0);
    }
    while (d.pos < a)
        slc_dec_next(s, &d);
    *hint = d;
    for (i = 0; i < n; i++)
        c[i] = (int8_t)(slc_dec_next(s, &d) >> 8);
    {
        int64_t bn = 0, be = 1;                     /* the best so far: bn / be = sc^2 / E (sc > 0) */
        uint32_t pass, lo2 = a, hi2 = b, st = 2u;
        best = UINT32_MAX;
        for (pass = 0; pass < 3u; pass++) {         /* pos itself; every 2nd; then the best's neighbours */
            if (pass == 1u)
                lo2 = a, hi2 = b, st = 2u;
            else if (pass == 2u) {
                if (best == UINT32_MAX)
                    break;
                lo2 = best > a ? best - 1u : a;
                hi2 = best + 1u < b ? best + 1u : b;
                st = 2u;
            } else
                lo2 = hi2 = pos;
            for (o = lo2; o <= hi2; o += st) {
                int32_t sc = 0, e = 1;
                if (pass && o == pos)
                    continue;
                for (j = 0; j < SHD_WW; j += 2u) {
                    int32_t x = c[o - a + j];
                    sc += r[j] * x;
                    e += x * x;
                }
                if (sc > 0 && (int64_t)sc * sc * be > bn * e + (pass ? bn * e >> 5 : 0)) {
                    bn = (int64_t)sc * sc;          /* (another start must beat pos's by 1 / 32) */
                    be = e;
                    best = o;
                }
            }
        }
        if (best == UINT32_MAX)
            best = pos;
    }
    return best;
}

/* one output sample of the voice's grains into *y; 0 when they are over (the head done, no grain sounding).
 * loop: LOOP held (the head goes round); ping: DIR PING; one: ONE (FREEZE holds as long again); play: PLAY */
static int shd_gr_tick(track_t *t, voice_t *v, const slc_src_t *s, shd_vx_t *x, const shd_gs_t *gs, int32_t *y,
                       int loop, int ping, int one, int play)
{
    shd_mem_t *mem = shd_mem(t);
    shd_gv_t *gv = shd_gv(t, v);
    uint32_t i, live = 0, hi = gs->b - gs->span, fz = gv->rev ? gs->a : hi;
    int32_t acc = 0;
    if (gv->first) {                                /* the slice's first grains: on its first sample, at their peak */
        uint32_t k, half = gs->len / 2u, at = gv->rev ? hi : gs->a;
        gv->first = 0;
        for (k = 0; k * gs->hop <= half && k < SHD_GN; k++)
            shd_gr_spawn(mem, gv, s, gs, at, half - k * gs->hop, gs->prate, 0);
        gv->wait = k * gs->hop > half ? k * gs->hop - half : gs->hop;
    }
    if (gv->run) {                                  /* the head */
        if (gv->held) {                             /* FREEZE: held while the key is; ONE: as long again; PLAY: on */
            if (play)
                ;
            else if (one ? !gv->hold || !--gv->hold : !v->gate)
                gv->run = 0;
        } else if (!gv->rev) {
            uint32_t f = gv->hfrac + gs->trate, end = gv->mode == SHD_GM_FREEZE ? fz : gs->b;
            gv->head += f >> 16;
            gv->hfrac = f & 0xFFFFu;
            while (gv->hd.pos < gv->head && gv->hd.pos + 1u < s->len) {   /* its decoder, keeping states */
                if (!(gv->hd.pos & (SHD_GCPS - 1u)))
                    gv->cp[(gv->hd.pos / SHD_GCPS) % SHD_GCP] = slc_dec_st(&gv->hd);
                slc_dec_next(s, &gv->hd);
            }
            if (gv->mode != SHD_GM_FREEZE && !loop && !ping && gv->head < end &&
                ((uint64_t)(end - gv->head) << 16) < (uint64_t)(gs->len / 2u) * gs->trate)
                gv->wait = gs->hop;                 /* (the last grains end with the slice: no new one past it) */
            if (gv->head >= end) {
                if (gv->mode == SHD_GM_FREEZE) {
                    gv->head = end;
                    gv->held = 1;
                    gv->hold = (uint32_t)((((uint64_t)(gs->b - gs->a)) << 16) / (gs->trate ? gs->trate : 1u));
                    gv->hold = gv->hold > 441000u ? 441000u : gv->hold;
                } else if (ping && (loop || !x->pinged)) {
                    gv->head = gs->b;               /* PING: back (once; LOOP: to and fro) */
                    gv->hfrac = 0;
                    gv->rev = 1;
                    x->pinged = 1;
                } else if (loop)
                    shd_gr_rewind(gv);
                else
                    gv->run = 0;
            }
        } else {
            uint32_t d = (gv->hfrac + 0x10000u - (gs->trate & 0xFFFFu)), back = (gs->trate >> 16) + 1u - (d >> 16);
            gv->hfrac = d & 0xFFFFu;
            if (gv->head < fz + back || gv->head < back) {
                if (gv->mode == SHD_GM_FREEZE) {
                    gv->head = fz;
                    gv->held = 1;
                    gv->hold = (uint32_t)((((uint64_t)(gs->b - gs->a)) << 16) / (gs->trate ? gs->trate : 1u));
                    gv->hold = gv->hold > 441000u ? 441000u : gv->hold;
                } else if (ping && loop)
                    shd_gr_rewind(gv);              /* PING: forwards again */
                else if (loop && !ping) {
                    gv->head = gs->b;               /* REV, LOOP: round from the end */
                    gv->hfrac = 0;
                } else {
                    gv->head = gs->a;
                    gv->run = 0;
                }
            } else
                gv->head -= back;
        }
    }
    if (gv->run) {                                  /* the next grain */
        if (gv->wait)
            gv->wait--;
        else {
            uint32_t r = (uint32_t)noise32(&mem->grng), h = gv->head, rate = gs->prate, w = gs->hop;
            int64_t pos;
            h = h < gs->a ? gs->a : h > gs->b ? gs->b : h;
            if (gv->mode == SHD_GM_CLOUD) {
                int32_t jd = (int32_t)(gs->jit * 16u / 127u), dt;
                pos = (int64_t)h - gs->span / 2u + (gs->scat ? (int64_t)(r % (2u * gs->scat + 1u)) - gs->scat : 0);
                dt = jd ? (int32_t)((r >> 20) % (2u * (uint32_t)jd + 1u)) - jd : 0;
                rate = (rate >> 8) * (pow2_q16(dt) >> 8);
                w = gs->hop / 2u + (r >> 8) % (gs->hop ? gs->hop : 1u);
            } else if (gv->rev)
                pos = (int64_t)h - gs->span + (gs->scat ? r % (gs->scat + 1u) : 0u);
            else
                pos = (int64_t)h - (gs->scat ? r % (gs->scat + 1u) : 0u);
            pos = pos < gs->a ? gs->a : pos > hi ? hi : pos;
            if (gv->mode != SHD_GM_CLOUD) {
                slc_dec_t hint;
                uint32_t p2 = shd_gr_align(mem, gv, s, (uint32_t)pos, gs->a, hi, &hint);
                shd_gr_spawn(mem, gv, s, gs, p2, 0, rate, hint.pos == UINT32_MAX ? 0 : &hint);
            } else
                shd_gr_spawn(mem, gv, s, gs, (uint32_t)pos, 0, rate, 0);
            gv->wait = w ? w - 1u : 0u;
        }
    }
    for (i = 0; i < SHD_GN; i++) {                  /* the grains */
        shd_gr_t *g = &gv->g[i];
        uint32_t nw;
        if (!g->winc)
            continue;
        live = 1;
        acc += ((g->a + (((g->b - g->a) * (int32_t)(g->frac >> 1)) >> 15)) * gr_win(g->wph)) >> 15;
        g->frac += g->rate;
        if (g->frac >= 65536u) {
            slc_dec_t d;
            d.pos = g->pos;
            d.pred = g->pred;
            d.idx = g->idx;
            d.seg = g->seg;
            do {
                g->a = g->b;
                g->b = d.pos < s->len ? (int16_t)slc_dec_next(s, &d) : 0;
                g->frac -= 65536u;
            } while (g->frac >= 65536u);
            g->pos = d.pos;
            g->pred = (int16_t)d.pred;
            g->idx = (uint8_t)d.idx;
            g->seg = (uint8_t)d.seg;
        }
        nw = g->wph + g->winc;
        if (nw < g->wph)
            g->winc = 0;
        else
            g->wph = nw;
    }
    *y = (int32_t)(((int64_t)acc * gs->gain) >> 15);
    return live || gv->run;
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
    shd_gv_t *gv = shd_gv(t, v);
    shd_gs_t gs;
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
    if (gv->mode)
        shd_gr_setup(t, v, s, x, m, &gs, play);
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
            if (gv->mode)
                shd_gr_setup(t, v, s, x, m, &gs, play);
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
        if (gv->mode) {                             /* GRAIN */
            if (!shd_gr_tick(t, v, s, x, &gs, &y, mode == SHD_M_LOOP && v->gate, sl->rev == SHD_PING, mode == SHD_M_ONE,
                             play)) {
                x->idle = 1;
                continue;
            }
        } else {
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
        }
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
            y = shd_shape(sl->dtype, y, sl->drv, &x->ds);
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
        } else if (!gv->mode) {                     /* the last ~1.5 ms of a one-shot (grains end in their windows) */
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
        if (sl->route == SHD_DIRECT || sl->route >= SHD_NROUTE) {
            out[i] += q;
            if (shd_swell) {                        /* (ARP's FEEDBACK SWELL: DIRECT feeds FULL as it rises) */
                mem->bus[SHD_NCH][i] += (int32_t)(((int64_t)q * (int32_t)shd_swell) >> 7);
                mem->used |= 1u << SHD_NCH;
            }
        } else {
            mem->bus[sl->route - 1u][i] += q;
            mem->used |= 1u << (sl->route - 1u);
        }
    }
    v->ph[1] = frac;
    if (gv->mode)
        v->ph[0] = gv->head;                        /* (LOOP's playheads) */
    else if (!((uint32_t)v->s[4] >> 7 & 1u)) {
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
    mem->gbud = mem->gbud + SHD_GBUD > SHD_GBUD_MAX ? SHD_GBUD_MAX : mem->gbud + SHD_GBUD;   /* GRAIN: the seeking, */
    mem->gload = 0;                                 /* the grains sounding */
    for (b = 0; b < NVOICE; b++)
        if (t->v[b].active && mem->gv[b].mode)
            for (i = 0; i < SHD_GN; i++)
                mem->gload += mem->gv[b].g[i].winc != 0u;
    if (!mem->grng)
        mem->grng = 0x2545F491;
}

/* after the voices: the multiband distortion over the buses, into the part */
static uint32_t shard_post(track_t *t, int32_t *out, uint32_t n)
{
    shd_mem_t *mem = shd_mem(t);
    const shd_bus_t *bs = &shd_kit[slc_src_of(t->p)].bus;
    int32_t al = SHD_ONEPOLE[bs->lo & 127], ah = SHD_ONEPOLE[bs->hi & 127], mix = clamp(t->p[P_E6], 0, 127);
    int32_t bias = clamp(t->p[P_E7], -64, 63);
    uint32_t i, b, dr[SHD_NCH], hot = 0;
    int32_t fg[SHD_NCH];
    for (b = 0; b < SHD_NCH; b++)                   /* (FDBK: a loop still ringing keeps the bus on) */
        hot |= (bs->fb[b] || shd_swell) && (mem->fbb[b] > 512 || mem->fbb[b] < -512);   /* (the band: no DC) */
    if (mem->used || hot)
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
    for (b = 0; b < SHD_NCH; b++) {
        uint32_t a = bs->fb[b] > shd_swell ? bs->fb[b] : shd_swell;   /* (ARP's FEEDBACK SWELL: at least this) */
        dr[b] = (uint32_t)clamp((int32_t)bs->drive[b] + bias, 0, 127);
        fg[b] = (int32_t)(a * a * 4u);
    }
    for (i = 0; i < n; i++) {
        int32_t xx = shd_sat(mem->bus[0][i], 1 << 17), split[3], wet = 0, dc;
        mem->lpl += (int32_t)(((int64_t)(xx - mem->lpl) * al) >> 15);
        mem->lph += (int32_t)(((int64_t)(xx - mem->lph) * ah) >> 15);
        split[0] = mem->lpl;
        split[1] = mem->lph - mem->lpl;
        split[2] = xx - mem->lph;
        for (b = 0; b < SHD_NCH; b++) {             /* LOW MID HIGH: the split + their own; FULL: its own */
            int32_t in = (b < 3u ? split[b] : 0) + mem->bus[b + 1u][i], a, tg, y;
            if (fg[b]) {                            /* FDBK: the last out, band-passed at the channel's own */
                int32_t hi;                         /* pitch (Q 2), back in (the shaper limits the howl) */
                mem->fbl[b] = shd_sat(mem->fbl[b] + ((SHD_FB_F[b] * mem->fbb[b]) >> 14), 1 << 17);
                hi = shd_sat(mem->fby[b] - mem->fbl[b] - (mem->fbb[b] >> 1), 1 << 17);
                mem->fbb[b] = shd_sat(mem->fbb[b] + ((SHD_FB_F[b] * hi) >> 14), 1 << 17);
                {
                    int32_t o = mem->fao[b] > 512 ? mem->fao[b] : 512, cq = (int32_t)(((int64_t)mem->fai[b] << 13) / o);
                    cq = cq > 32768 ? 32768 : cq;   /* (Q13: the shaper's gain undone, up to 4; below -36 dB out less: the loop dies away in the quiet) */
                    in += (int32_t)(((((int64_t)mem->fbb[b] * fg[b]) >> 15) * cq) >> 13);
                }
            }
            in = shd_sat(in, 1 << 17);
            a = in < 0 ? -in : in;
            mem->fol[b] += a > mem->fol[b] ? (a - mem->fol[b]) >> 4 : -(mem->fol[b] >> 11);
            tg = !bs->gate[b] || mem->fol[b] > SHD_GATE_Q15[bs->gate[b] & 127] ? 32768 : 0;
            mem->gain[b] += (tg - mem->gain[b]) >> 7;
            y = shd_shape(bs->type[b], in, dr[b], &mem->ds[b]);
            mem->fby[b] = fg[b] ? shd_sat(y, 1 << 16) : 0;   /* (before the gate: the loop howls on through it) */
            if (fg[b]) {                            /* (means over ~3 ms) */
                mem->fai[b] += (a - mem->fai[b]) >> 7;
                mem->fao[b] += ((y < 0 ? -y : y) - mem->fao[b]) >> 7;
            }
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

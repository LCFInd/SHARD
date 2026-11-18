/* SPDX-License-Identifier: GPL-3.0-only */
/* DECK MODE (2026-11-17, the mangler): tracks 1 and 2 are decks A and B. After the engine, each deck's part runs
 * through its own isolator, then the crossfader (fx.c mix_part) takes them:
 *
 *   x -> split at XLO and XHI, 12 dB/oct (Linkwitz-Riley, each crossover two trapezoidal one-poles: LP = y2,
 *        HP = x - 2 y1 + y2, LP - HP the first-order all-pass 2 y1 - x): LOW = LP at XLO, then XHI's all-pass; MID = LP at XHI of minus
 *        HP at XLO; HIGH = HP at XHI of HP at XLO. The three add up to x through two all-passes (flat: only the phase
 *        turns), and a band at KILL is gone 12 dB/oct beyond its crossover
 *     -> each band: its FEEDBACK (FB LO / FBMID / FB HI, ARP's FEEDBACK SWELL at least, PRESS pushing it): the band's
 *        last output band-passed at its own pitch (150 Hz, 900 Hz, 4 kHz, moved by PITCH and ARP's PITCH) back into
 *        it, the band held by a soft clip (shd_soft) while it feeds back; ARP's PANIC stops every loop for ~0.5 s
 *     -> each band's level: LO MID HI, KILL .. 0 dB (64) .. +6 dB (shard_tables.h DECK_GAIN_Q14)
 *     -> the sum through one shaper (TYPE, any of SHARD's; DRIVE, plus EDIT 2's DRIVE on a SHARD deck)
 *     -> the DJ filter (FILT: left of 0 a low-pass closing, right a high-pass opening; SHARD's state-variable filter
 *        at 2x, a little resonance)
 *     -> a DC blocker (only behind a shaper or feedback), then on a SHARD deck MULTI (E6) mixes dry and this.
 *
 * Untouched (every band 0 dB, TYPE OFF, FILT OFF, no feedback) a deck is its input bit for bit and costs nothing.
 * Its values: core.h P_DK_* (the FM operator values every track stores); this replaces SHARD's per-source bus
 * (eng_shard.c shard_post, which the mangler no longer runs: every slice plays DIRECT into its part, its ROUTE, the
 * FULL channel and the gates gone). Tracks 3 and 4 play into the mix as they are. */

typedef struct {
    int32_t a1, a2, b1, b2, c1;                     /* the split: XLO's one-poles' states, XHI's, XHI's all-pass on LOW */
    int32_t fl[3], fb[3], fy[3];                    /* FEEDBACK: each band's band-pass (low, band), its last output */
    int32_t sl, sb;                                 /* the DJ filter (low, band) */
    int32_t dcx, dcy;
    shd_ds_t ds;                                    /* the shaper's state */
    uint32_t tail, kseen, kleft;                    /* blocks of tail left; PANIC seen, blocks of feedback off */
    uint8_t live;                                   /* last block was processed (else its states start again) */
} deck_st_t;
static deck_st_t deck_st[NDECK];
/* the ISOLATE page's analyzer (ui_graph.c): the watched deck's last DECK_SN samples in and out (the UI sets
 * deck_watch to 1 + the deck each frame it shows; none: nothing is kept) */
#define DECK_SN 256u
static volatile uint8_t deck_watch;
static int16_t deck_sin[DECK_SN], deck_sout[DECK_SN];
static volatile uint32_t deck_sw;
static void deck_take(uint32_t d, const int32_t *in, const int32_t *out, uint32_t n)
{
    uint32_t i, w = deck_sw;
    if (deck_watch != d + 1u)
        return;
    for (i = 0; i < n; i++, w++) {
        deck_sin[w & (DECK_SN - 1u)] = (int16_t)shd_sat(in ? in[i] >> 1 : out[i] >> 1, 32767);
        deck_sout[w & (DECK_SN - 1u)] = (int16_t)shd_sat(out[i] >> 1, 32767);
    }
    deck_sw = w;
}

/* a trapezoidal one-pole (TPT): its state s, coefficient G (Q15) -> the low-pass of x */
static inline int32_t deck_tpt(int32_t x, int32_t *s, int32_t G)
{
    int32_t v = (int32_t)(((int64_t)(x - *s) * G) >> 15), y = v + *s;
    *s = y + v;
    return y;
}
/* a deck's band levels, Q14 (KILL 0 .. 16384 0 dB .. +6 dB) */
static inline int32_t deck_gain(const int16_t *p, uint32_t b) { return DECK_GAIN_Q14[clamp(p[P_DK_LO + b], 0, 127)]; }

/* the deck untouched: nothing to do (and no state to keep) */
static int deck_flat(const track_t *t)
{
    const int16_t *p = t->p;
    return p[P_DK_LO] == 64 && p[P_DK_MID] == 64 && p[P_DK_HI] == 64 && !p[P_DK_TYPE] && !p[P_DK_FILT] &&
           p[P_DK_FBLO] <= 0 && p[P_DK_FBMID] <= 0 && p[P_DK_FBHI] <= 0 && !shd_swell;
}

/* engine post (voice.c track_render): part t's block through its deck; nr: voices rendered into it. 1 = it sounds */
static uint32_t deck_post(track_t *t, int32_t *out, uint32_t n, uint32_t nr)
{
    deck_st_t *d = &deck_st[(uint32_t)(t - trk) % NDECK];
    const int16_t *p = t->p;
    int32_t al, ah, g[3], fg[3], ff[3], sf = 0, sq = 0, mix = 127;
    uint32_t i, b, hot = 0, type = (uint32_t)clamp(p[P_DK_TYPE], 0, SHD_NDRIVE - 1), drv, pr;
    int32_t filt = clamp(p[P_DK_FILT], -64, 63);
    if (deck_flat(t)) {
        d->live = 0;
        d->tail = 0;
        if (nr)
            deck_take((uint32_t)(t - trk) % NDECK, 0, out, n);   /* (the analyzer: in = out) */
        return 0;
    }
    if (d->kseen != shd_kill) {                     /* PANIC: every loop stopped, feedback off a while */
        d->kseen = shd_kill;
        d->kleft = SHD_KILL_BLOCKS;
        memset(d->fl, 0, sizeof d->fl);
        memset(d->fb, 0, sizeof d->fb);
        memset(d->fy, 0, sizeof d->fy);
    }
    pr = (uint32_t)t->at * (uint32_t)clamp(p[P_DK_PRESS], 0, 127) / 127u;   /* PRESS: the keys' pressure */
    for (b = 0; b < 3u; b++) {
        uint32_t a = (uint32_t)clamp(p[P_DK_FBLO + b], 0, 127);
        int32_t pt = clamp(p[P_DK_PITCH], -24, 24) * 16 + shd_fbpitch;
        a = a > shd_swell ? a : shd_swell;
        if (a && pr)
            a = a + pr / 2u > 127u ? 127u : a + pr / 2u;
        if (d->kleft)
            a = 0;
        fg[b] = (int32_t)(a * a * 4u);              /* (as SHARD's FDBK: from about 64 the loop holds itself) */
        ff[b] = SHD_FB_F[b];
        if (pt) {
            int32_t f = (int32_t)(((int64_t)SHD_FB_F[b] * pow2_q16(clamp(pt, -768, 768))) >> 16);
            ff[b] = f < 40 ? 40 : f > 22937 ? 22937 : f;
        }
        hot |= fg[b] && (d->fb[b] > 512 || d->fb[b] < -512);
        g[b] = deck_gain(p, b);
    }
    if (d->kleft)
        d->kleft--;
    if (nr || hot)
        d->tail = 64u;                              /* ~46 ms of shaper, filter and loop tails */
    else if (!d->tail)
        return 0;
    else
        d->tail--;
    if (!d->live) {                                 /* from untouched: the filters start where the signal is */
        d->a1 = d->a2 = d->c1 = 2 * out[0];         /* (a TPT state at rest on x is 2 x) */
        d->b1 = d->b2 = 0;
        d->sl = d->sb = d->dcx = d->dcy = 0;
        memset(&d->ds, 0, sizeof d->ds);
        d->live = 1;
    }
    al = DECK_TPT_G[clamp(p[P_DK_XLO], 0, 127)];
    ah = DECK_TPT_G[clamp(p[P_DK_XHI], 0, 127)];
    if (ah < al) {
        int32_t tmp = al;
        al = ah;
        ah = tmp;
    }
    drv = (uint32_t)clamp(p[P_DK_DRIVE] + (t->engine == ENGI_SHARD ? p[P_E7] : 0) + (int32_t)(pr / 2u), 0, 127);
    if (filt) {                                     /* the DJ filter: LP 64 .. 1 from the top down, HP 1 .. 63 up */
        uint32_t c = filt < 0 ? (uint32_t)(127 + filt * 2) : (uint32_t)(filt * 2);
        sf = SHD_SVF_F[c & 127];
        sq = SHD_SVF_Q[40] > SHD_SVF_QMAX[c & 127] ? SHD_SVF_QMAX[c & 127] : SHD_SVF_Q[40];
    }
    if (t->engine == ENGI_SHARD)
        mix = clamp(p[P_E6], 0, 127);
    if (deck_watch == (uint32_t)(t - trk) % NDECK + 1u)   /* (the analyzer: the input, before it is overwritten) */
        for (i = 0; i < n && i < DECK_SN; i++)
            deck_sin[(deck_sw + i) & (DECK_SN - 1u)] = (int16_t)shd_sat(out[i] >> 1, 32767);
    for (i = 0; i < n; i++) {
        int32_t x = shd_sat(out[i], 1 << 19), band[3], y = 0, r, y1, y2, z1, z2;
        y1 = deck_tpt(x, &d->a1, al);
        y2 = deck_tpt(y1, &d->a2, al);
        r = -(x - 2 * y1 + y2);                       /* minus XLO's high-pass */
        z1 = deck_tpt(r, &d->b1, ah);
        z2 = deck_tpt(z1, &d->b2, ah);
        band[0] = 2 * deck_tpt(y2, &d->c1, ah) - y2;  /* XLO's low-pass through XHI's all-pass */
        band[1] = z2;
        band[2] = -(r - 2 * z1 + z2);
        for (b = 0; b < 3u; b++) {
            int32_t v = band[b];
            if (fg[b]) {                            /* FEEDBACK: the last output, band-passed (Q 2), back in */
                int32_t hi;
                d->fl[b] = shd_sat(d->fl[b] + ((ff[b] * d->fb[b]) >> 14), 1 << 17);
                hi = shd_sat(d->fy[b] - d->fl[b] - (d->fb[b] >> 1), 1 << 17);
                d->fb[b] = shd_sat(d->fb[b] + ((ff[b] * hi) >> 14), 1 << 17);
                v = shd_soft(shd_sat(v + (int32_t)(((int64_t)d->fb[b] * fg[b]) >> 15), 3 << 15));
                d->fy[b] = v;
            }
            y += (int32_t)(((int64_t)v * g[b]) >> 14);
        }
        if (type)
            y = shd_shape(type, y, drv, &d->ds);
        if (filt) {                                 /* two steps a sample (the table is for 2x) */
            uint32_t k;
            for (k = 0; k < 2u; k++) {
                int32_t h;
                d->sl = shd_sat(d->sl + (int32_t)(((int64_t)d->sb * sf) >> 14), 1 << 20);
                h = shd_sat(y - d->sl - (int32_t)(((int64_t)d->sb * sq) >> 14), 1 << 20);
                d->sb = shd_sat(d->sb + (int32_t)(((int64_t)h * sf) >> 14), 1 << 20);
                if (k)
                    y = filt < 0 ? d->sl : h;
            }
        }
        if (type || fg[0] | fg[1] | fg[2]) {        /* DC blocker, ~10 Hz (a lopsided shaper, a loop) */
            int32_t dc = y - d->dcx + (int32_t)(((int64_t)d->dcy * 32700) >> 15);
            d->dcx = y;
            d->dcy = dc;
            y = dc;
        }
        out[i] = mix >= 127 ? y : (int32_t)((((int64_t)x * (127 - mix) + (int64_t)shd_sat(y, 1 << 20) * mix) * 516) >> 16);
    }
    if (deck_watch == (uint32_t)(t - trk) % NDECK + 1u) {   /* (the analyzer: the output beside its input) */
        for (i = 0; i < n && i < DECK_SN; i++)
            deck_sout[(deck_sw + i) & (DECK_SN - 1u)] = (int16_t)shd_sat(out[i] >> 1, 32767);
        deck_sw += n;
    }
    return 1;
}

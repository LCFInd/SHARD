/* SPDX-License-Identifier: GPL-3.0-only */
/* DECK MODE (firmware/src/deck.c, the mangler build): decks A and B (tracks 1 and 2) through their isolators.
 *   1. untouched: the part bit for bit; one band 0.1 dB off: flat (the split's bands add up through all-passes)
 *   2. KILL: LO at 100 Hz, HI at 10 kHz, MID at 1 kHz gone (12 dB/oct), the other bands kept
 *   3. the DJ filter: LP closes the top, HP the bottom; every TYPE at full DRIVE bounded
 *   4. tracks 3 and 4 never go through a deck
 *   5. a project of before (the FM operators' defaults there) converts: the deck defaults, decks on SHARD with
 *      their kit's crossovers, FDBK and PRESS; idempotent
 * Run by tests/run_host_tests.sh (the mangler's flags). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
#include "../firmware/src/project.c"
#if !FELUCCA_MANGLER
#error "deck_test needs the mangler (DECK MODE)"
#endif

static int fails;
static void check(const char *what, int ok, const char *fmt, double a, double b)
{
    printf("deck: %-74s %s", what, ok ? "ok" : "FAIL");
    if (fmt) {
        printf("  (");
        printf(fmt, a, b);
        printf(")");
    }
    printf("\n");
    fails += !ok;
}

static track_t *const A = &trk[0];
static void deck_reset(void)
{
    uint32_t i;
    memset(deck_st, 0, sizeof deck_st);
    for (i = P_FM1_ATK; i <= P_FM4_LEVEL; i++)
        A->p[i] = TP[i].def;
    A->engine = A->eng_req = ENGI_GRAIN;                /* (not SHARD: no MULTI, no EDIT 2 DRIVE) */
    shd_swell = 0;
    shd_fbpitch = 0;
}
/* a sine of hz at amp through deck A for a second; the output's rms over the last half against the input's */
static double deck_db(double hz, double amp)
{
    int32_t b[CTL];
    uint32_t f, i;
    double ei = 0, eo = 0, ph = 0;
    memset(deck_st, 0, sizeof deck_st);
    for (f = 0; f < FS; f += CTL) {
        for (i = 0; i < CTL; i++, ph += 2 * M_PI * hz / FS)
            b[i] = (int32_t)(amp * sin(ph));
        if (f >= FS / 2u)
            for (i = 0; i < CTL; i++)
                ei += (double)b[i] * b[i];
        deck_post(A, b, CTL, 1);
        if (f >= FS / 2u)
            for (i = 0; i < CTL; i++)
                eo += (double)b[i] * b[i];
    }
    return 10 * log10((eo + 1e-9) / (ei + 1e-9));
}

int main(void)
{
    uint32_t i, k;
    host_tracks_init();
    host_preset(A, ENGI_GRAIN, 0);

    /* 1 */
    deck_reset();
    {
        int32_t b[CTL], c[CTL];
        for (i = 0; i < CTL; i++)
            b[i] = c[i] = (int32_t)(i * 997u % 20000u) - 10000;
        check("untouched: nothing done, the part bit for bit", deck_post(A, b, CTL, 1) == 0 && !memcmp(b, c, sizeof b),
              0, 0, 0);
    }
    A->p[P_DK_HI] = 65;                                 /* (+0.1 dB: the split runs) */
    {
        double lo = deck_db(60, 8000), mid = deck_db(1000, 8000), hi = deck_db(9000, 8000);
        check("one band 0.1 dB up: flat through the split (60 Hz, 1 kHz within 0.3 dB, 9 kHz +0.1)",
              fabs(lo) < 0.3 && fabs(mid) < 0.3 && hi > -0.2 && hi < 0.4, "%.2f %.2f", lo, mid);
    }

    /* 2 */
    deck_reset();
    A->p[P_DK_LO] = 0;
    {
        double k100 = deck_db(100, 8000), keep = deck_db(8000, 8000);
        check("LO KILL: 100 Hz down over 18 dB (XLO 400 Hz), 8 kHz kept", k100 < -18 && fabs(keep) < 1.0,
              "%.1f dB, %.2f dB", k100, keep);
    }
    deck_reset();
    A->p[P_DK_HI] = 0;
    {
        double k10 = deck_db(10000, 8000), keep = deck_db(100, 8000);
        check("HI KILL: 10 kHz down over 18 dB (XHI 3 kHz), 100 Hz kept", k10 < -18 && fabs(keep) < 1.0,
              "%.1f dB, %.2f dB", k10, keep);
    }
    deck_reset();
    A->p[P_DK_MID] = 0;
    {
        double k1 = deck_db(1100, 8000), lo = deck_db(60, 8000), hi = deck_db(12000, 8000);
        check("MID KILL: 1.1 kHz down over 10 dB, 60 Hz and 12 kHz within 1.5 dB", k1 < -10 && fabs(lo) < 1.5 &&
              fabs(hi) < 1.5, "%.1f dB, %.2f dB", k1, lo);
    }
    deck_reset();
    A->p[P_DK_LO] = A->p[P_DK_MID] = A->p[P_DK_HI] = 127;
    {
        double u = deck_db(1000, 4000);
        check("every band +6 dB: +6 dB", u > 5.5 && u < 6.5, "%.2f dB", u, 0);
    }

    /* 3 */
    deck_reset();
    A->p[P_DK_FILT] = -40;
    {
        double top = deck_db(8000, 8000), low = deck_db(100, 8000);
        check("FILT LP 40: 8 kHz down over 20 dB, 100 Hz within 1.5 dB", top < -20 && fabs(low) < 1.5,
              "%.1f dB, %.2f dB", top, low);
    }
    deck_reset();
    A->p[P_DK_FILT] = 40;
    {
        double low = deck_db(60, 8000), top = deck_db(8000, 8000);
        check("FILT HP 40: 60 Hz down over 20 dB, 8 kHz within 1.5 dB", low < -20 && fabs(top) < 1.5,
              "%.1f dB, %.2f dB", low, top);
    }
    {
        int ok = 1;
        int32_t worst = 0;
        for (k = 1; k < SHD_NDRIVE; k++) {
            int32_t b[CTL];
            uint32_t f;
            deck_reset();
            A->p[P_DK_TYPE] = (int16_t)k;
            A->p[P_DK_DRIVE] = 127;
            A->p[P_DK_LO] = A->p[P_DK_MID] = A->p[P_DK_HI] = 127;
            for (f = 0; f < FS / 4u; f += CTL) {
                for (i = 0; i < CTL; i++)
                    b[i] = (int32_t)(300000 * sin(2 * M_PI * 80.0 * (f + i) / FS));
                deck_post(A, b, CTL, 1);
                for (i = 0; i < CTL; i++)
                    worst = abs(b[i]) > worst ? abs(b[i]) : worst;
            }
            ok &= worst < (1 << 20);
        }
        check("every TYPE at full DRIVE, every band +6 dB, a loud input: bounded", ok, "peak %.0f", worst, 0);
    }

    /* 4 */
    {
        uint32_t ok = 1;
        for (k = 0; k < NTRK; k++)
            ok &= (uint32_t)deck_of(&trk[k]) == (k < 2u);
        check("decks: tracks 1 and 2 only", ok, 0, 0, 0);
    }
    /* 5 */
    {
        static project_t q;
        uint32_t t;
        int fresh_ok = 1, kit_ok, again;
        memset(&q, 0, sizeof q);
        q.magic = PROJ_MAGIC;
        q.size = sizeof q;
        for (t = 0; t < NTRK; t++) {                    /* a project of before: the FM operators' defaults */
            for (k = 0; k < 20u; k++)
                q.t[t].p[P_FM1_ATK + k] = (int16_t)(k % 5u == 2u || k % 5u == 4u ? 127 : 0);
            q.t[t].engine = ENGI_SHARD;
            q.t[t].p[P_E0] = 1;                         /* USR1 */
        }
        q.t[1].engine = ENGI_GRAIN;
        shd_kit[1].bus.lo = 40;
        shd_kit[1].bus.hi = 100;
        shd_kit[1].bus.fb[1] = 77;
        shd_kit[1].bus.press = 9;
        q.sum = proj_sum(&q);
        proj_deck(&q);
        for (t = 1; t < NTRK; t++)
            for (k = P_FM1_ATK; k <= P_FM4_LEVEL; k++)
                fresh_ok &= q.t[t].p[k] == TP[k].def;
        kit_ok = q.t[0].p[P_DK_XLO] == 40 && q.t[0].p[P_DK_XHI] == 100 && q.t[0].p[P_DK_FBMID] == 77 &&
                 q.t[0].p[P_DK_PRESS] == 9 && q.t[0].p[P_DK_LO] == 64 && q.t[0].p[P_DK_CURVE] == 0;
        check("a project of before: every track the deck defaults (deck B on GRAIN, tracks 3 and 4 too)", fresh_ok,
              0, 0, 0);
        check("a project of before: deck A on SHARD takes its kit's crossovers, FDBK and PRESS", kit_ok, 0, 0, 0);
        q.t[0].p[P_DK_XLO] = 7;
        q.sum = proj_sum(&q);
        proj_deck(&q);
        again = q.t[0].p[P_DK_XLO] == 7 && q.sum == proj_sum(&q);
        check("converted once: a later load leaves it (and its sum is right)", again, 0, 0, 0);
    }

    printf(fails ? "deck test FAILED (%d)\n" : "deck test passed\n", fails);
    return fails ? 1 : 0;
}

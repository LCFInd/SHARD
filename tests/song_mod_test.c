/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD's song sections bring their slot's modulation (song_chain.c CHAIN_SND, 2026-11-19): a song of two sections,
 * slot A then slot B, each saved with its own MOD matrix, LFO, AMP ENV and MOD ENV: each section plays with its
 * slot's, the steps as before; STOP brings back the values from before PLAY. The rest of the sound stays.
 *   build/host/song_mod_test */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static int bad;
static void ck(const char *what, int ok)
{
    printf("song mod: %-86s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void set(track_t *t, int src, int dst, int amt, int rate, int atk, int menv, int lvl)
{
    t->p[P_M1SRC] = (int16_t)src; t->p[P_M1DST] = (int16_t)dst; t->p[P_M1AMT] = (int16_t)amt;
    t->p[P_LRATE] = (int16_t)rate; t->p[P_ATK] = (int16_t)atk; t->p[P_MENV_D] = (int16_t)menv;
    t->p[P_LEVEL] = (int16_t)lvl;
}
static int is(const track_t *t, int src, int dst, int amt, int rate, int atk, int menv)
{
    return t->p[P_M1SRC] == src && t->p[P_M1DST] == dst && t->p[P_M1AMT] == amt && t->p[P_LRATE] == rate &&
           t->p[P_ATK] == atk && t->p[P_MENV_D] == menv;
}
int main(void)
{
    int32_t o[2 * CTL];
    uint32_t b, k, r1 = 0;
    track_t *t = &trk[1];
    ui_power_on();
    for (k = 0; k < NTRK; k++)
        trk[k].p[P_SLEN] = 4;                       /* (short sections) */
    set(t, MS_MENV, MD_XLO, 40, 30, 10, 50, 100);
    ck("slot A saved", project_save(0) == 0);
    set(t, MS_LFO, MD_CUT, -20, 90, 60, 20, 90);
    ck("slot B saved", project_save(1) == 0);
    set(t, MS_VEL, MD_PAN, 7, 64, 5, 70, 80);       /* the music as it is before PLAY */
    chain_config.count = 2;
    for (k = 0; k < NTRK; k++) {
        chain_config.row[0].slot[k] = 0;
        chain_config.row[1].slot[k] = 1;
    }
    chain_config.row[0].repeat = chain_config.row[1].repeat = 1;
    ck("the song starts", chain_prepare() == 0);
    for (b = 0; b < 20u && !chain.running; b++)
        mix_block(o, CTL);
    ck("section 1: slot A's matrix, LFO, AMP ENV and MOD ENV", chain.running && chain.row == 0 && is(t, MS_MENV, MD_XLO, 40, 30, 10, 50));
    ck("section 1: the rest of the sound stays (LEVEL)", t->p[P_LEVEL] == 80);
    for (b = 0; b < 20000u && chain.running && chain.row == 0; b++)
        mix_block(o, CTL);
    r1 = chain.row;
    ck("section 2: slot B's", chain.running && r1 == 1 && is(t, MS_LFO, MD_CUT, -20, 90, 60, 20));
    transport_req = 2;
    for (b = 0; b < 20u && chain.running; b++)
        mix_block(o, CTL);
    ck("STOP: the values from before PLAY", !chain.running && is(t, MS_VEL, MD_PAN, 7, 64, 5, 70) && t->p[P_LEVEL] == 80);
    printf(bad ? "song mod test FAILED (%d)\n" : "song mod test passed\n", bad);
    return bad != 0;
}

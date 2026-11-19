/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD's 2ND CORE SPLIT (fx.c mix_pipe, 2026-11-19) on the host: every part's voices on this core, each part's rest
 * (DECK MODE's isolators, DIST, INSERT, the SLICER, the sends) on a thread that plays core 1, one part behind. A
 * scene with everything that keeps state in a part's rest: both decks with their bands cut and shaped, feedback, the
 * DJ filter, MENV moving XLO and DRIVE (the matrix's values in t->p while the thread reads them), STRUM with SPREAD,
 * an INSERT, the SLICER, DIST, the MASTER EQ. Rendered on one core, then SPLIT, then SPLIT with core 1 failing on
 * its 300th job (retired: its part's rest done here, one core after): bit for bit the same. Built with
 * -fsanitize=thread (run_tests.sh TSAN=1): nothing touched by both cores at once.
 *   build/host/split_test */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define FELUCCA_CPU2 1

static pthread_mutex_t c1t_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c1t_cv = PTHREAD_COND_INITIALIZER;
static void (*c1t_fn)(uint32_t);
static uint32_t c1t_arg, c1t_job, c1t_done, c1t_up, c1t_taken, c1t_fail_at;
static int c1t_mode, c1t_dropped;                   /* 0 one core, 1 a thread as core 1; dropped: the failed job */

static void *c1t_main(void *x)
{
    (void)x;
    pthread_mutex_lock(&c1t_mu);
    for (;;) {
        void (*fn)(uint32_t);
        uint32_t arg;
        while (c1t_done == c1t_job)
            pthread_cond_wait(&c1t_cv, &c1t_mu);
        fn = c1t_fn;
        arg = c1t_arg;
        pthread_mutex_unlock(&c1t_mu);
        if (!c1t_dropped)
            fn(arg);
        pthread_mutex_lock(&c1t_mu);
        c1t_done = c1t_job;
        pthread_cond_broadcast(&c1t_cv);
    }
    return 0;
}

static int c1t_run(void (*fn)(uint32_t), uint32_t arg)
{
    if (!c1t_mode)
        return 0;
    if (!c1t_up) {
        pthread_t th;
        if (pthread_create(&th, 0, c1t_main, 0))
            return 0;
        c1t_up = 1;
    }
    pthread_mutex_lock(&c1t_mu);
    c1t_fn = fn;
    c1t_arg = arg;
    c1t_job++;
    c1t_taken++;
    c1t_dropped = c1t_fail_at && c1t_taken == c1t_fail_at;   /* (a misread on the device: the job not run) */
    pthread_cond_broadcast(&c1t_cv);
    pthread_mutex_unlock(&c1t_mu);
    return 1;
}

static int c1t_wait(void)                           /* 0 done; -1 retired, the job dropped (do it yourself) */
{
    int r;
    pthread_mutex_lock(&c1t_mu);
    while (c1t_done != c1t_job)
        pthread_cond_wait(&c1t_cv, &c1t_mu);
    r = c1t_dropped ? -1 : 0;
    if (r)
        c1t_mode = 0;                               /* retired: one core from now on */
    pthread_mutex_unlock(&c1t_mu);
    return r;
}

#define C1_ON() (c1t_mode != 0)
#define C1_RUN(fn, arg) c1t_run(fn, arg)
#define C1_WAIT() c1t_wait()
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <sys/wait.h>
#include <unistd.h>

#define SECS 4u
static int32_t peak[NPART];

static void setup(void)
{
    track_t *a = &trk[0], *b = &trk[1], *c = &trk[2], *d = &trk[3];
    uint32_t k;
    host_tracks_init();
    host_preset(a, ENGI_SHARD, 0);
    host_preset(b, ENGI_SAMPLE, 1);
    host_preset(c, ENGI_GRAIN, 2);
    host_preset(d, ENGI_SAMPLE, 3);
    for (k = 0; k < NPART; k++)
        trk[k].p[P_MUTE] = 0;
    a->p[P_DK_LO] = 90;                             /* deck A: bands cut and shaped, feedback, the filter */
    a->p[P_DK_MID] = 20;
    a->p[P_DK_LTYPE] = 2;
    a->p[P_DK_LDRV] = 70;
    a->p[P_DK_HTYPE] = 1;
    a->p[P_DK_FBMID] = 80;
    a->p[P_DK_FILT] = -25;
    a->p[P_MENV_A] = 30;                            /* MENV -> XLO, -> DRIVE (t->p while core 1 reads it) */
    a->p[P_MENV_D] = 70;
    a->p[P_MENV_S] = 40;
    a->p[P_M1SRC] = MS_MENV;
    a->p[P_M1DST] = MD_XLO;
    a->p[P_M1AMT] = 40;
    a->p[P_M2SRC] = MS_MENV;
    a->p[P_M2DST] = MD_BDRV;
    a->p[P_M2AMT] = 30;
    b->p[P_VOICE] = V_STRUM;                        /* deck B: STRUM with SPREAD, shapers, feedback */
    b->p[P_SPRD] = 100;
    b->p[P_DK_HI] = 30;
    b->p[P_DK_MTYPE] = 3;
    b->p[P_DK_MDRV] = 90;
    b->p[P_DK_FBLO] = 60;
    b->p[P_DK_FILT] = 30;
    b->p[P_REV] = 40;
    c->p[P_ITYPE] = 2;                              /* GRAIN: an INSERT and the SLICER */
    c->p[P_IMIX] = 90;
    c->p[P_SLCR] = 1;
    c->p[P_SLDEPTH] = 100;
    c->p[P_DLY] = 50;
    d->p[P_DIST] = 60;                              /* SAMPLE: DIST, the matrix's LFO on CUT, a send */
    d->p[P_LRATE] = 100;
    d->p[P_M1SRC] = MS_LFO;
    d->p[P_M1DST] = MD_CUT;
    d->p[P_M1AMT] = 30;
    d->p[P_CHOR] = 30;
    trk[0].p[P_DK_XFADE] = 10;                      /* the crossfader off the middle; the MASTER EQ on */
    trk[0].p[P_MEQ_LO] = 4;
    trk[0].p[P_MEQ_MID] = -3;
    trk[0].p[P_MEQ_HI] = 6;
}

static uint64_t render(int mode, uint32_t fail_at)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t f, k, i;
    int32_t o[2 * CTL];
    setup();
    c1t_mode = mode;
    c1t_fail_at = fail_at;
    for (f = 0; f < SECS * FS; f += CTL) {
        uint32_t ms = f / (FS / 1000u);
        for (k = 0; k < NPART; k++) {
            uint32_t ph = (ms + 250u * k) % 900u, note = 45u + 7u * k + (ms / 900u) % 3u * 2u;
            if (ph == 0u)
                trk_note_on(&trk[k], note, 100);
            if (ph == 600u)
                trk_note_off(&trk[k], note);
        }
        mix_block(o, CTL);
        for (k = 0; k < NPART; k++)
            if (trk[k].peak > peak[k])
                peak[k] = trk[k].peak;
        for (i = 0; i < 2u * CTL; i++) {
            uint32_t u = (uint32_t)o[i];
            for (k = 0; k < 4u; k++) {
                h ^= (u >> (8u * k)) & 0xFFu;
                h *= 0x100000001B3ull;
            }
        }
    }
    return h;
}

static uint64_t render_fork(int mode, uint32_t fail_at, uint32_t *taken)
{
    int fd[2];
    uint64_t h = 0;
    pid_t pid;
    if (pipe(fd))
        return 0;
    fflush(stdout);
    pid = fork();
    if (!pid) {
        h = render(mode, fail_at);
        if (write(fd[1], &h, sizeof h) != sizeof h || write(fd[1], peak, sizeof peak) != sizeof peak ||
            write(fd[1], &c1t_taken, sizeof c1t_taken) != sizeof c1t_taken)
            _exit(2);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], &h, sizeof h) != sizeof h || read(fd[0], peak, sizeof peak) != sizeof peak ||
        read(fd[0], taken, sizeof *taken) != sizeof *taken)
        h = 0;
    close(fd[0]);
    waitpid(pid, 0, 0);
    return h;
}

int main(void)
{
    uint32_t taken = 0, k, quiet = 0, fails = 0;
    uint64_t ref = render_fork(0, 0, &taken), h;
    for (k = 0; k < NPART; k++)
        quiet += peak[k] < 200;
    printf("split: one core: peaks %d %d %d %d%s\n", peak[0], peak[1], peak[2], peak[3], quiet ? "   FAIL (a silent part)" : "");
    fails += quiet != 0;
    h = render_fork(1, 0, &taken);
    printf("split: SPLIT, core 1 taking every part's rest (%u jobs): %s\n", taken,
           h == ref && taken >= NPART * (SECS * FS / CTL) ? "the same, bit for bit   ok" : "FAIL");
    fails += !(h == ref && taken >= NPART * (SECS * FS / CTL));
    h = render_fork(1, 300, &taken);
    printf("split: SPLIT, core 1 failing at its 300th job (retired: one core after): %s\n",
           h == ref && taken == 300u ? "the same, bit for bit   ok" : "FAIL");
    fails += !(h == ref && taken == 300u);
    printf(fails ? "split test FAILED (%u)\n" : "split test passed\n", fails);
    return fails != 0;
}

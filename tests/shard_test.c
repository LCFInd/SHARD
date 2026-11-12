/* SPDX-License-Identifier: GPL-3.0-only */
/* SHARD engine tests (same sources as the firmware, through hostsim.c; run_tests.sh):
 *   build/host/shard_test LOOP DEMO_DIR      (LOOP: as slice_test's, tests/slice_loop.py + fm1_sample_upload.py build)
 * 1. the engine table: SHARD is 15, 14 reserved and never offered, shown after SLICE.
 * 2. ONE / GATE / LOOP on BREAK: a note plays its slice; ONE to its end, GATE stops at the release.
 * 3. PLAY: the slices start where they sit in the loop, scaled to the tempo (120 and 90 BPM), the loop repeats,
 *    the pitch does not change with the tempo; BARS AUTO puts BREAK at 120 BPM.
 * 4. per-slice settings: PITCH speeds the slice up, REV plays it backwards (== SLICE's REV), LEVEL, DECAY ends it,
 *    every FILTER and DRIVE bounded, CHOKE ends the group's other voice.
 * 5. routing: MULTI with MIX 0 == DIRECT bit for bit; MULTI / LOW / MID / HIGH with every shaper at full drive bounded;
 *    the bus goes quiet after the voices.
 * 6. the kit's store with the user slot's sample (slice_store.c): written when asked and stopped, read back at a scan,
 *    the MAN slices' record beside it intact, junk ignored, a kit for another sample ignored.
 * 7. demos into DEMO_DIR: BREAK in PLAY at 120 and 90 BPM, a resequenced kit with routing, the user loop in PLAY. */
#include <stdarg.h>
#include <stdint.h>
static uint32_t host_slots[3u * 0x14000u / 4u];          /* USR1..3, as the flash at 0xA0000 */
#define SMP_USER_XIP(k) ((const uint8_t *)host_slots + (k) * SMP_USER_SIZE)
#define main hostsim_main
#include "hostsim.c"
#undef main
#if !FELUCCA_SHARD
#error "shard_test needs the SHARD engine (FELUCCA_SHARD=1, the default)"
#endif

static uint8_t flash_ok = 1, host_busy, host_page;
static char host_msg[32];
static int transport_busy(void) { return host_busy; }
static int slice_page_on(void) { return host_page; }
static void ui_message(const char *s) { snprintf(host_msg, sizeof host_msg, "%s", s); }
static int st_read(uint32_t off, void *dst, uint32_t n) { (void)off; (void)dst; (void)n; return -1; }
static int st_erase(uint32_t off)
{
    if (off < SMP_USER_BASE || off + 0x1000u > SMP_USER_BASE + sizeof host_slots || (off & 0xFFFu))
        return -8;
    memset((uint8_t *)host_slots + (off - SMP_USER_BASE), 0xFF, 0x1000u);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off < SMP_USER_BASE || off + n > SMP_USER_BASE + sizeof host_slots)
        return -8;
    for (i = 0; i < n; i++)
        ((uint8_t *)host_slots)[off - SMP_USER_BASE + i] &= ((const uint8_t *)src)[i];
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/slice_store.c"

static int fails;
static void check(const char *what, int ok, const char *fmt, ...)
{
    printf("shard: %-60s %s", what, ok ? "ok" : "FAIL");
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        printf("  (");
        vprintf(fmt, ap);
        printf(")");
        va_end(ap);
    }
    printf("\n");
    fails += !ok;
}

static track_t *const T = &trk[0];
static void voices_off(void)
{
    uint32_t i, k;
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NVOICE; i++)
            trk[k].v[i].active = 0;
}
static uint32_t nvoices(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += T->v[i].active;
    return n;
}
static voice_t *first_voice(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (T->v[i].active)
            return &T->v[i];
    return 0;
}
/* blocks of the mix; the largest |sample| of the left channel, the energy */
static int32_t out_peak;
static double out_energy;
static void blocks(uint32_t n)
{
    int32_t o[2 * CTL];
    uint32_t i;
    while (n--) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++) {
            int32_t a = abs(o[2 * i]);
            out_peak = a > out_peak ? a : out_peak;
            out_energy += (double)o[2 * i] * o[2 * i];
        }
    }
}
static void fresh(uint32_t preset, int16_t mode, int16_t div)
{
    voices_off();
    host_tracks_init();
    host_preset(T, ENGI_SHARD, preset);
    T->p[P_E4] = mode;
    T->p[P_E1] = div;
    T->p[P_DIST] = T->p[P_CHOR] = T->p[P_DLY] = T->p[P_REV] = 0;
    shd_init();
    blocks(4);
    out_peak = 0;
    out_energy = 0;
}

/* PLAY: render frames, log every slice change of the first voice (frame, slice) */
static uint32_t log_slices(uint32_t frames, uint32_t *at, uint32_t *sl, uint32_t max)
{
    uint32_t f, n = 0, last = 255;
    for (f = 0; f < frames; f += CTL) {
        voice_t *v = first_voice();
        if (v) {
            uint32_t j = shd_slice_of(v);
            if (j != last && n < max) {
                at[n] = f;
                sl[n++] = j;
                last = j;
            }
        }
        blocks(1);
    }
    return n;
}

/* run fn in a child (the parent's state as it is: the same start for two renders), its samples back in buf */
static void in_child(void (*fn)(uint32_t, int32_t *), uint32_t arg, int32_t *buf, uint32_t n)
{
    char tmp[] = "/tmp/shard_child_XXXXXX";
    int fd = mkstemp(tmp);
    pid_t pid;
    fflush(stdout);
    if (!(pid = fork())) {
        fn(arg, buf);
        if (write(fd, buf, n * sizeof *buf)) {}
        _exit(0);
    }
    waitpid(pid, 0, 0);
    lseek(fd, 0, SEEK_SET);
    if (read(fd, buf, n * sizeof *buf)) {}
    close(fd);
    unlink(tmp);
}
#define RUN_N (FS / 2u)
static void run_route(uint32_t route, int32_t *buf)
{
    int32_t o[2 * CTL];
    uint32_t f, i;
    T->p[P_E6] = 0;                                            /* MULTI mix 0: the bus dry */
    for (i = 0; i < SLC_AUTO; i++)
        shd_kit[0].sl[i].route = (uint8_t)route;
    trk_note_on(T, 60, 110);
    trk_note_on(T, 64, 110);
    for (f = 0; f + CTL <= RUN_N; f += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++)
            buf[f + i] = o[2 * i];
    }
}
static void run_tempo(uint32_t bpm, int32_t *buf)
{
    int32_t o[2 * CTL];
    uint32_t f, i;
    song.g[G_BPM] = (int16_t)bpm;
    trk_note_on(T, 60, 110);
    for (f = 0; f + CTL <= RUN_N; f += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++)
            buf[f + i] = o[2 * i];
    }
}

static long load(const char *path, void *dst, long max)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return -1;
    n = (long)fread(dst, 1, (size_t)max, f);
    fclose(f);
    return n;
}

/* 7: a demo: track 1 on SHARD (BREAK unless src), steps, bars at bpm, written to dir/file */
static void demo(const char *dir, const char *file, uint32_t bpm, uint32_t bars, int16_t mode, int16_t div, int16_t src,
                 const uint8_t *notes, uint32_t nsteps)
{
    char path[512];
    FILE *w;
    uint32_t f, i, frames, bar;
    int32_t peak = 0;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (!(w = fopen(path, "wb")))
        return;
    song.g[G_BPM] = (int16_t)bpm;
    T->p[P_E4] = mode;
    T->p[P_E1] = div;
    T->p[P_E0] = src;
    for (i = 0; i < NSTEP; i++)
        T->step[i].time = ST_REST;
    for (i = 0; i < nsteps; i++) {
        uint8_t n = notes[i];
        put_step(T, i, n ? 1u : 0u, &n, n ? ST_NOTE : ST_REST, 0u);
    }
    T->p[P_SLEN] = (int16_t)nsteps;
    bar = (uint32_t)(4.0 * 60.0 / bpm * FS);
    frames = bars * bar + FS;
    wav_hdr(w, frames);
    transport_req = 1;
    for (f = 0; f < frames; f += CTL) {
        int32_t o[2 * CTL];
        if (f >= bars * bar && f < bars * bar + CTL)
            transport_req = 2;
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++) {
            int32_t a = abs(o[2 * i]);
            peak = a > peak ? a : peak;
            wav_put(w, o[2 * i], o[2 * i + 1]);
        }
    }
    fclose(w);
    printf("shard: demo %-34s %u bars at %u BPM, peak %d\n", file, bars, bpm, peak);
    check("demo bounded and audible", peak > 2000 && peak < 32767, "%s", file);
}


/* 8: tempo. A user slot (k) from 16-bit PCM at 22.05 kHz, IMA ADPCM as tools/sampleio.py, then scanned */
static void slot_pcm(uint32_t k, const int16_t *x, uint32_t n)
{
    smp_user_hdr_t *h = (smp_user_hdr_t *)((uint8_t *)host_slots + k * SMP_USER_SIZE);
    uint8_t *d = (uint8_t *)h + SMP_USER_DATA;
    int32_t pred = 0, idx = 0;
    uint32_t i;
    memset(h, 0, SMP_USER_SIZE);
    n = n < 2u * (SMP_USER_SIZE - SMP_USER_DATA) ? n : 2u * (SMP_USER_SIZE - SMP_USER_DATA);   /* (what a slot holds) */
    for (i = 0; i < n; i++) {
        int32_t step = IMA_STEP[idx], diff = x[i] - pred, vd = step >> 3, code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; vd += step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; vd += step >> 1; }
        if (diff >= step >> 2) { code |= 1; vd += step >> 2; }
        pred = clamp(code & 8 ? pred - vd : pred + vd, -32768, 32767);
        idx = clamp(idx + IMA_IDX[code & 7], 0, 88);
        d[i >> 1] |= (uint8_t)(i & 1u ? code << 4 : code);
    }
    h->magic = SMP_USER_MAGIC;
    h->version = 1;
    h->nz = 1;
    memcpy(h->name, "TEMPO", 5);
    h->data_len = (n + 1u) / 2u;
    h->zone[0].n = n;
    h->zone[0].le = n - 1u;
    h->zone[0].rate = 32768u;
    h->zone[0].root16 = 60 * 16;
    h->zone[0].hi = 127;
    smp_user_scan(k);
}
/* drums at 22.05 kHz: 0 kick, 1 snare, 2 closed hat, added at t seconds */
static uint32_t rnd_s = 1;
static double rnd1(void) { rnd_s = rnd_s * 1103515245u + 12345u; return ((rnd_s >> 8) & 0xFFFF) / 32768.0 - 1.0; }
static void hit(double *x, uint32_t n, double t, int what, double g)
{
    uint32_t i0 = (uint32_t)(t * 22050.0), i;
    double ph = 0, hp = 0, prev = 0;
    for (i = 0; i < 22050u * 4u / 10u && i0 + i < n; i++) {
        double u = i / 22050.0, v;
        if (what == 0) { ph += 2 * M_PI * (50 + 120 * exp(-u * 40)) / 22050.0; v = sin(ph) * exp(-u * 9); }
        else if (what == 1) { double r = rnd1(); v = (r * 0.8 + sin(2 * M_PI * 190 * u) * 0.4) * exp(-u * 18); }
        else { double r = rnd1(); hp = r - prev; prev = r; v = hp * exp(-u * 70) * 0.6; }
        x[i0 + i] += v * g;
    }
}
/* pattern: 16 steps a bar, a char per step: K kick, S snare, H hat, X kick + hat, Y snare + hat, . rest */
static uint32_t make_loop(double *x, uint32_t cap, double bpm, const char *pat, uint32_t bars, double tail)
{
    double six = 60.0 / bpm / 4.0;
    uint32_t n = (uint32_t)((bars * 16 * six + tail) * 22050.0), b, i, len = (uint32_t)strlen(pat);
    n = n < cap ? n : cap;
    memset(x, 0, n * sizeof *x);
    for (b = 0; b < bars; b++)
        for (i = 0; i < 16u; i++) {
            char c = pat[(b * 16u + i) % len];
            double t = (b * 16u + i) * six;
            if (c == 'K' || c == 'X') hit(x, n, t, 0, 1.0);
            if (c == 'S' || c == 'Y') hit(x, n, t, 1, 0.8);
            if (c == 'H' || c == 'X' || c == 'Y') hit(x, n, t, 2, 0.5);
        }
    return n;
}
static uint32_t to_pcm(const double *x, int16_t *y, uint32_t n)
{
    double pk = 1e-9;
    uint32_t i;
    for (i = 0; i < n; i++) pk = fabs(x[i]) > pk ? fabs(x[i]) : pk;
    for (i = 0; i < n; i++) y[i] = (int16_t)(x[i] / pk * 30000.0);
    return n;
}

int main(int argc, char **argv)
{
    const char *loop = argc > 1 ? argv[1] : "build/slice_demo/loop";
    const char *dir = argc > 2 ? argv[2] : "build/shard_demo";
    char path[512];
    uint32_t i, at[64], sl[64], n, k, src;
    int ok;

    /* 1 */
    check("SHARD is engine 15, after DRUM in the shown order", ENGINES[ENGI_SHARD] == &ENG_SHARD &&
          eng_rank(ENGI_SHARD) == eng_rank(ENGI_DRUM) + 1u, 0);
    check("engine 14 reserved without ADPCM: never offered", FELUCCA_ADPCM || (!eng_ok(14) && NENG_SHOWN == 14u),
          "%u shown", NENG_SHOWN);
    for (ok = 1, i = 0; i < NENG_SHOWN; i++)
        ok &= eng_vis(i) != 14u || FELUCCA_ADPCM;
    check("the order lists no reserved engine", ok, 0);

    /* the user loop into USR1, as at boot (slc_store_boot: the scans read the stored slices and kits) */
    slc_store_boot();
    snprintf(path, sizeof path, "%s.hdr", loop);
    n = (uint32_t)load(path, host_slots, SMP_USER_DATA);
    snprintf(path, sizeof path, "%s.bin", loop);
    if (n != sizeof(smp_user_hdr_t) || load(path, (uint8_t *)host_slots + SMP_USER_DATA, SMP_USER_SIZE - SMP_USER_DATA) <= 0) {
        printf("shard: no user loop (%s.hdr / .bin)\n", loop);
        return 1;
    }
    shd_init();
    smp_user_scan(0);
    check("USR1 scanned", slc_get(1) != 0, "%u samples", slc_usr[0].len);
    check("tempo: USR1's loop (2 bars at 96 BPM, swung) found as 96", shd_det_bpm10[1] >= 955u && shd_det_bpm10[1] <= 965u,
          "%u.%u BPM", shd_det_bpm10[1] / 10u, shd_det_bpm10[1] % 10u);
    {   /* 8: tempo of made loops in USR3 */
        static double x[22050 * 8];
        static int16_t y[22050 * 8];
        static const struct { const char *name, *pat; double bpm, tail; uint32_t bars, lo10, hi10, alt_lo10, alt_hi10; } L[] = {
            {"4 on the floor 128, 2 bars", "XHHHXHHHXHHHXHHH", 128, 0, 2, 1278, 1282, 0, 0},
            {"house 124, offbeat hats, 2 bars", "K.H.K.H.K.H.K.H.", 124, 0, 2, 1238, 1242, 0, 0},
            {"break 95, 2 bars", "K.H.S.HKH.KHS.H.", 95, 0, 2, 948, 952, 0, 0},
            {"hip hop 88, 2 bars", "K..HS.K.H.KHS..H", 88, 0, 2, 878, 882, 1758, 1762},
            {"drum and bass 174, 4 bars", "K.H.S.HHH.K.S.H.", 174, 0, 4, 1738, 1742, 868, 872},
            {"techno 140, 1 bar", "XHHHYHHHXHHHYHHH", 140, 0, 1, 1398, 1402, 0, 0},
            {"112, 1 bar with a 0.3 s tail (not whole beats)", "K.H.S.H.K.H.S.H.", 112, 0.3, 1, 1100, 1140, 0, 0},
            {"slow 72, 1 bar", "K...S...K.K.S...", 72, 0, 1, 718, 722, 1438, 1442},
        };
        for (i = 0; i < NELEM(L); i++) {
            uint32_t m = make_loop(x, NELEM(x), L[i].bpm, L[i].pat, L[i].bars, L[i].tail), f;
            slot_pcm(2, y, to_pcm(x, y, m));
            f = shd_det_bpm10[3];
            snprintf(path, sizeof path, "tempo: %s", L[i].name);
            check(path, (f >= L[i].lo10 && f <= L[i].hi10) || (f >= L[i].alt_lo10 && f <= L[i].alt_hi10),
                  "%u.%u BPM", f / 10u, f % 10u);
        }
        for (i = 0; i < 22050u * 3u; i++)                              /* a pad: no tempo */
            y[i] = (int16_t)(sin(2 * M_PI * 220 * i / 22050.0) * 12000 * (0.6 + 0.4 * sin(2 * M_PI * 0.3 * i / 22050.0)));
        slot_pcm(2, y, 22050u * 3u);
        check("tempo: a held tone has none", shd_det_bpm10[3] == 0u, "%u", shd_det_bpm10[3]);
        /* PLAY's loop from the tempo: 2 bars at 128 played at 128 = the sample's own length (x 2: 22.05 kHz) */
        {
            uint32_t m = make_loop(x, NELEM(x), 128, "XHHHXHHHXHHHXHHH", 2, 0), l;
            slot_pcm(2, y, to_pcm(x, y, m));
            fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
            T->p[P_E0] = 3;
            song.g[G_BPM] = 128;
            l = shd_loop_len(T->p, slc_get(3));
            check("tempo: BARS AUTO plays the found tempo's loop at the song's (128 at 128: its own length)",
                  l + 16u >= 2u * m && l <= 2u * m + 16u, "%u for %u", l, 2u * m);
            shd_kit[3].bus.bpm = 64;
            l = shd_loop_len(T->p, slc_get(3));
            check("tempo: the kit's BPM 64 overrides it (half the beats: half as long)", l + 16u >= m && l <= m + 16u, "%u", l);
            shd_kit[3].bus.bpm = 0;
            T->p[P_E5] = 2;                                            /* BARS 2: as set, the tempo aside */
            l = shd_loop_len(T->p, slc_get(3));
            check("tempo: BARS 2 is two bars at the song's tempo", l == 8u * beat_samples(), "%u", l);
        }
        {   /* 2, 6, 12 equal slices: on the grid, exact, with the right decoder state */
            static const uint8_t C[3] = {SLC_DIV_2, SLC_DIV_6, SLC_DIV_12}, N[3] = {2, 6, 12};
            const slc_src_t *u = slc_get(3);
            uint32_t c, a, b, st;
            for (ok = 1, c = 0; c < 3u; c++) {
                ok &= slc_count(u, C[c]) == N[c];
                for (k = 0; k < N[c]; k++) {
                    slc_bounds(u, C[c], k, &a, &b, &st);
                    ok &= a == k * u->len / N[c] && b == (k + 1u) * u->len / N[c] && st == slc_state_at(u, a);
                }
            }
            check("DIV 2 / 6 / 12: equal slices, exact starts and decoder states", ok, 0);
            for (ok = 1, c = 0; c < SHD_NDIV; c++)
                ok &= c >= SHD_DIV_AUTO || slc_count(u, SHD_DIV_CODE[c]) == (uint32_t)atoi(N_SHD_DIV[c]);
            check("SHARD's DIV list: 2 4 6 8 12 16 32 slices", ok, 0);
        }
        memset((uint8_t *)host_slots + 2u * SMP_USER_SIZE, 0, SMP_USER_SIZE);
        smp_user_scan(2);
        check("tempo: an emptied slot has none", shd_det_bpm10[3] == 0u, 0);
    }

    /* 2 */
    fresh(1, SHD_M_ONE, SHD_DIV_AUTO);
    trk_note_on(T, 61, 110);                                   /* slice 1 of BREAK's AUTO slices */
    blocks(1);
    check("ONE: a note plays its slice", nvoices() == 1u && shd_slice_of(first_voice()) == 1u, 0);
    trk_note_off(T, 61);
    blocks(400);
    check("ONE: the slice plays to its end, then the voice ends", nvoices() == 0u && out_peak > 2000, "peak %d", out_peak);
    fresh(1, SHD_M_GATE, 5);
    trk_note_on(T, 60, 110);
    blocks(20);
    trk_note_off(T, 60);
    blocks(100);
    check("GATE: the release ends the voice", nvoices() == 0u, 0);
    fresh(1, SHD_M_LOOP, 6);                                   /* 32 slices: short ones */
    trk_note_on(T, 60, 110);
    blocks(2000);
    check("LOOP: a held slice keeps sounding", nvoices() == 1u, 0);
    trk_note_off(T, 60);
    blocks(200);
    check("LOOP: ends after the release", nvoices() == 0u, 0);

    /* 3: PLAY */
    for (k = 0; k < 2u; k++) {
        uint32_t bpm = k ? 90u : 120u, nb = slc_count(&SLC_BREAK, SLC_DIV_AUTO), worst = 0;
        double scale = (double)FS * 120.0 / bpm / ((double)((SLC_BREAK.rate >> 4) * 44100u >> 12));
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
        song.g[G_BPM] = (int16_t)bpm;
        check("PLAY: BARS AUTO puts BREAK at 1 bar (120 BPM)", shd_bars(T->p, &SLC_BREAK) == 1u, 0);
        trk_note_on(T, 60, 110);
        n = log_slices(FS * 2u * 120u / bpm + FS / 2u, at, sl, 64);
        ok = n > nb;
        for (i = 0; i < n && i < nb; i++) {
            double expect = SLC_BREAK.apos[i] * scale;
            uint32_t e = (uint32_t)fabs(at[i] - expect);
            ok &= sl[i] == i && e <= 2u * CTL;
            worst = e > worst ? e : worst;
        }
        check(k ? "PLAY 90 BPM: every slice at its place scaled (within 2 blocks)" :
                  "PLAY 120 BPM: every slice at its place (within 2 blocks)", ok, "%u changes, worst %u samples", n, worst);
        check("PLAY: round the loop to slice 1 again", n > nb && sl[nb] == 0u &&
              fabs(at[nb] - (double)FS * 2.0 * 120.0 / bpm) <= 2.0 * CTL, "at %u", n > nb ? at[nb] : 0);
        trk_note_off(T, 60);
        blocks(200);
    }
    {   /* the pitch does not follow the tempo: the first slice (before 120 BPM's second) the same at 120 and 90 */
        static int32_t a[RUN_N], b[RUN_N];
        uint32_t f, diff = 0, upto = (uint32_t)(SLC_BREAK.apos[1] * (double)FS / ((SLC_BREAK.rate >> 4) * 44100u >> 12)) - 64u;
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
        in_child(run_tempo, 120, a, RUN_N);
        in_child(run_tempo, 90, b, RUN_N);
        for (f = 0; f < upto; f++)
            diff += a[f] != b[f];
        check("PLAY: the tempo moves the slices, not the pitch (slice 1 alike)", diff == 0u, "%u of %u differ", diff, upto);
    }

    /* 4: per-slice settings */
    {
        double e0, e1;
        fresh(1, SHD_M_ONE, 5);
        trk_note_on(T, 60, 110);
        blocks(200);
        e0 = out_energy;
        trk_note_off(T, 60);
        fresh(1, SHD_M_ONE, 5);
        shd_kit[0].sl[0].level = 50;
        trk_note_on(T, 60, 110);
        blocks(200);
        e1 = out_energy;
        check("LEVEL 50 is quieter than 100 (about -6 dB)", e1 < e0 * 0.4 && e1 > e0 * 0.1, "%.2f", e1 / e0);
    }
    {
        uint32_t len[2];
        for (k = 0; k < 2u; k++) {
            fresh(1, SHD_M_ONE, 5);
            shd_kit[0].sl[0].pitch = k ? 12 : 0;
            trk_note_on(T, 60, 110);
            for (len[k] = 0; nvoices() && len[k] < 2000u; len[k]++)
                blocks(1);
        }
        check("PITCH +12 plays the slice in half the time", len[1] * 2u >= len[0] - 2u && len[1] * 2u <= len[0] + 2u,
              "%u / %u blocks", len[1], len[0]);
    }
    {
        fresh(1, SHD_M_ONE, 5);
        shd_kit[0].sl[0].dec = 40;
        trk_note_on(T, 60, 110);
        for (n = 0; nvoices() && n < 2000u; n++)
            blocks(1);
        check("DECAY 40 ends the slice early", n < 60u, "%u blocks", n);
    }
    {   /* REV: the voice reads backwards: its first source sample is the slice's last */
        voice_t *v;
        uint32_t a, b, st;
        fresh(1, SHD_M_ONE, 5);
        shd_kit[0].sl[3].rev = 1;
        trk_note_on(T, 63, 110);
        v = first_voice();
        slc_bounds(&SLC_BREAK, 2, 3, &a, &b, &st);
        check("REV: starts at the slice's end, reads down", v && ((uint32_t)v->s[4] >> 7 & 1u) && v->ph[0] == b, 0);
        blocks(2);
        check("REV: moving down", v && v->ph[0] < b && v->ph[0] > a, 0);
    }
    for (ok = 1, k = 0; k < 4u; k++) {
        uint32_t ty;
        for (ty = 0; ty < SHD_NDRIVE; ty++) {
            fresh(1, SHD_M_LOOP, 5);
            for (i = 0; i < SLC_AUTO; i++) {
                shd_kit[0].sl[i].filt = (uint8_t)k;
                shd_kit[0].sl[i].res = 127;
                shd_kit[0].sl[i].cut = (uint8_t)(30 * k + 10);
                shd_kit[0].sl[i].dtype = (uint8_t)ty;
                shd_kit[0].sl[i].drv = 127;
            }
            for (i = 0; i < 8u; i++)
                trk_note_on(T, 60 + i, 127);
            blocks(300);
            ok &= out_peak > 100 && out_peak < 32767;
        }
    }
    check("every FILTER x DRIVE at full resonance and drive, 8 voices: bounded", ok, 0);
    {
        voice_t *v0;
        fresh(1, SHD_M_LOOP, 5);
        shd_kit[0].sl[0].choke = shd_kit[0].sl[1].choke = 2;
        trk_note_on(T, 60, 110);
        blocks(4);
        v0 = first_voice();
        trk_note_on(T, 61, 110);
        blocks(2);
        check("CHOKE: the group's other slice ends", v0 && !(v0->active && shd_slice_of(v0) == 0u) && nvoices() == 1u, 0);
    }

    /* 5: routing */
    {
        static int32_t a[RUN_N], b[RUN_N];
        uint32_t f, diff = 0;
        fresh(1, SHD_M_ONE, 5);
        in_child(run_route, SHD_DIRECT, a, RUN_N);
        in_child(run_route, SHD_MULTI, b, RUN_N);
        for (f = 0; f < RUN_N; f++)
            diff += a[f] != b[f];
        check("MULTI at MIX 0 == DIRECT, bit for bit", diff == 0u, "%u samples differ", diff);
    }
    for (ok = 1, k = SHD_MULTI; k < SHD_NROUTE; k++) {
        uint32_t ty;
        for (ty = 0; ty < SHD_NDRIVE; ty++) {
            fresh(1, SHD_M_LOOP, 5);
            T->p[P_E7] = 63;
            for (i = 0; i < 3u; i++) {
                shd_kit[0].bus.type[i] = (uint8_t)ty;
                shd_kit[0].bus.drive[i] = 127;
                shd_kit[0].bus.gate[i] = 30;
                shd_kit[0].bus.level[i] = 127;
            }
            for (i = 0; i < SLC_AUTO; i++)
                shd_kit[0].sl[i].route = (uint8_t)k;
            for (i = 0; i < 8u; i++)
                trk_note_on(T, 60 + i, 127);
            blocks(200);
            ok &= out_peak > 100 && out_peak < 32767;
        }
    }
    check("MULTI / LOW / MID / HIGH, every shaper at full drive: bounded", ok, 0);
    {
        shd_mem_t *mem;
        for (i = 0; i < 8u; i++)
            trk_note_off(T, 60 + i);
        blocks(400);
        mem = shd_mem(T);
        check("the bus stops after the voices (its tail run out)", nvoices() == 0u && !mem->tail && !mem->used,
              "%u voices, tail %u", nvoices(), mem->tail);
    }

    /* 6: the kit's store */
    src = 1u;
    shd_kit[src].sl[2].pitch = -7;
    shd_kit[src].sl[5].route = SHD_HIGH;
    shd_kit[src].bus.type[1] = SHD_D_DECIM;
    {
        slc_man_t *m = slc_man_begin(0);                       /* MAN slices too: both records in the sector */
        uint32_t nm = m ? m->n : 0;
        if (m) {
            slc_man_split(slc_get(1), m, 0);
            slc_man_commit(0);
            slc_man_save |= 1u;
        }
        shd_kit_save |= 1u;
        host_busy = 1;
        slc_store_poll();
        check("not written while playing", shd_kit_save == 1u, 0);
        host_busy = 0;
        slc_store_poll();
        check("written once stopped", !shd_kit_save && !slc_man_save, "%s", host_msg);
        shd_kit_default(&shd_kit[src]);
        smp_user_scan(0);                                      /* as at boot */
        check("the kit comes back with the sample", shd_kit[src].sl[2].pitch == -7 &&
              shd_kit[src].sl[5].route == SHD_HIGH && shd_kit[src].bus.type[1] == SHD_D_DECIM, 0);
        m = (slc_man_t *)slc_man_of(slc_get(1));
        check("the MAN slices beside it come back too", m && m->n == nm + 1u, "%u slices", m ? m->n : 0);
    }
    {
        uint8_t *rec = (uint8_t *)host_slots + SHD_REC_OFF;
        rec[40] ^= 0x55;                                       /* a damaged kit: its CRC fails */
        smp_user_scan(0);
        check("a damaged kit is ignored (neutral)", shd_kit[src].sl[2].pitch == 0, 0);
        rec[40] ^= 0x55;
        ((smp_user_hdr_t *)host_slots)->crc ^= 1u;             /* another sample: its kit is not this one's */
        smp_user_scan(0);
        ((smp_user_hdr_t *)host_slots)->crc ^= 1u;
        check("a kit for another sample is ignored", shd_kit[src].sl[2].pitch == 0, 0);
        smp_user_scan(0);
        check("the right sample again: its kit", shd_kit[src].sl[2].pitch == -7, 0);
    }
    {   /* junk in range: shd_kit_sane */
        shd_kit_t j;
        memset(&j, 0xEE, sizeof j);
        shd_kit_sane(&j);
        for (ok = 1, i = 0; i < SLC_AUTO; i++)
            ok &= j.sl[i].route < SHD_NROUTE && j.sl[i].dtype < SHD_NDRIVE && j.sl[i].mode <= SHD_LOOP &&
                  j.sl[i].pitch >= -24 && j.sl[i].pitch <= 24 && j.sl[i].filt <= SHD_F_BP && j.sl[i].choke <= 4u;
        check("a kit of junk reads in range", ok, 0);
    }

    /* 7: demos */
    {
        static const uint8_t ONE[16] = {60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const uint8_t CHOP[16] = {60, 61, 62, 60, 64, 62, 63, 61, 65, 60, 66, 67, 62, 62, 68, 69};
        char cmd[600];
        snprintf(cmd, sizeof cmd, "mkdir -p %s", dir);
        if (system(cmd)) {}
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
        T->p[P_SGATE] = 127;
        T->p[P_SLEN] = 16;
        demo(dir, "play_break_120.wav", 120, 4, SHD_M_PLAY, SHD_DIV_AUTO, 0, ONE, 16);
        demo(dir, "play_break_90.wav", 90, 4, SHD_M_PLAY, SHD_DIV_AUTO, 0, ONE, 16);
        fresh(1, SHD_M_ONE, 5);
        shd_kit[0].sl[1].rev = 1;
        shd_kit[0].sl[3].pitch = -5;
        shd_kit[0].sl[3].route = SHD_LOW;
        shd_kit[0].sl[4].filt = SHD_F_BP; shd_kit[0].sl[4].cut = 80; shd_kit[0].sl[4].res = 110;
        shd_kit[0].sl[5].route = SHD_MULTI;
        shd_kit[0].sl[6].dtype = SHD_D_FOLD; shd_kit[0].sl[6].drv = 90;
        shd_kit[0].sl[7].route = SHD_HIGH; shd_kit[0].sl[7].dec = 70;
        shd_kit[0].sl[8].pitch = 7; shd_kit[0].sl[8].route = SHD_MID;
        T->p[P_E7] = 20;
        demo(dir, "kit_break_routed.wav", 128, 4, SHD_M_ONE, 5, 0, CHOP, 16);
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
        T->p[P_SGATE] = 127;
        demo(dir, "play_usr_loop_110.wav", 110, 4, SHD_M_PLAY, SHD_DIV_AUTO, 1, ONE, 16);
    }
    printf(fails ? "shard: %d FAILED\n" : "shard: all passed\n", fails);
    return fails != 0;
}

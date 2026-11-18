#include <time.h>
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
static uint32_t host_slots[4u * 0x14000u / 4u];          /* USR1..3, as the flash at 0xA0000 */
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
/* the flash's slots in host_slots: USR1..3 from 0xA0000, USR4 (the mangler) at 0xE7000; 0 = not a slot */
static uint8_t *host_flash(uint32_t off, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        uint32_t b = k < 3u ? SMP_USER_BASE + k * 0x14000u : 0xE7000u;
        if (off >= b && off + n <= b + 0x14000u)
            return (uint8_t *)host_slots + k * 0x14000u + (off - b);
    }
    return 0;
}
static int st_erase(uint32_t off)
{
    uint8_t *p = host_flash(off, 0x1000u);
    if (!p || (off & 0xFFFu))
        return -8;
    memset(p, 0xFF, 0x1000u);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    uint8_t *p = host_flash(off, n);
    if (!p)
        return -8;
    for (i = 0; i < n; i++)
        p[i] &= ((const uint8_t *)src)[i];
    return 0;
}
/* bounce.c (the mangler): what it needs of the UI */
#if FELUCCA_MANGLER
static struct { uint8_t force; } ui;
static char host_said[40];
static void ui_say(const char *a, const char *b) { snprintf(host_said, sizeof host_said, "%s%s", a, b); }
static void ui_draw(void) {}
static void fm1_wdt_feed(void) {}
#endif
#include "../firmware/src/storage.c"
#include "../firmware/src/slice_store.c"
#if FELUCCA_MANGLER
#include "../firmware/src/bounce.c"
#endif

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

/* 10: GRAIN. A note's render (the left channel) into sg_buf, the key released at rel (frames; 0 = held); the frames
 * until the voices end (frames: did not) */
static int32_t sg_buf[FS * 3];
static uint32_t sg_run(uint32_t frames, uint32_t rel, uint8_t note)
{
    int32_t o[2 * CTL];
    uint32_t f, i, end = frames;
    trk_note_on(T, note, 110);
    for (f = 0; f + CTL <= frames; f += CTL) {
        if (rel && f <= rel && f + CTL > rel)
            trk_note_off(T, note);
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++)
            sg_buf[f + i] = o[2 * i];
        if (end == frames && !nvoices())
            end = f;
    }
    return end;
}
/* the tone's frequency over [a, b) (rising zero crossings), the peak over it */
static double sg_hz(uint32_t a, uint32_t b)
{
    uint32_t i, z = 0, z0 = 0, z1 = 0;
    for (i = a + 1u; i < b; i++)
        if (sg_buf[i - 1] < 0 && sg_buf[i] >= 0) {
            if (!z++)
                z0 = i;
            z1 = i;
        }
    return z > 1u ? (z - 1u) * (double)FS / (z1 - z0) : 0;
}
/* the strongest frequency in [lo, hi) Hz over [a, b) (Goertzel, 1 Hz): the pitch through grains' phase steps */
static double sg_tone(uint32_t a, uint32_t b, double lo, double hi)
{
    double f, best = 0, bp = -1;
    uint32_t i;
    for (f = lo; f < hi; f += 1.0) {
        double w = 2 * cos(2 * M_PI * f / FS), s0, s1 = 0, s2 = 0, p;
        for (i = a; i < b; i++) {
            s0 = sg_buf[i] + w * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        p = s1 * s1 + s2 * s2 - w * s1 * s2;
        if (p > bp) {
            bp = p;
            best = f;
        }
    }
    return best;
}
static int32_t sg_peak(uint32_t a, uint32_t b)
{
    int32_t p = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        p = abs(sg_buf[i]) > p ? abs(sg_buf[i]) : p;
    return p;
}
/* every slice of source src to GRAIN g (the slice's own), SIZE DENS JITTER */
static void sg_all(uint32_t src, uint8_t g, uint8_t size, uint8_t dens, uint8_t jit)
{
    uint32_t i;
    for (i = 0; i < SLC_AUTO; i++) {
        shd_kit[src].sl[i].grain = g;
        shd_kit[src].sl[i].gsize = size;
        shd_kit[src].sl[i].gdens = dens;
        shd_kit[src].sl[i].gjit = jit;
    }
}
static void sg_tests(void)
{
    static int16_t y[22050];
    static int32_t ref[FS];
    uint32_t i, d0, d1, d2, d3, sil, k;
    double h0, h1, h2, h3, c, e0, e1;
    int ok;
    /* the material: 1 s of 220 Hz in USR3 (source 3), two slices (DIV 2) of 0.5 s */
    for (i = 0; i < NELEM(y); i++)
        y[i] = (int16_t)(sin(2 * M_PI * 220 * i / 22050.0) * 16000);
    slot_pcm(2, y, NELEM(y));
#define GR_FRESH(mode) do { fresh(1, mode, 0); T->p[P_E0] = 3; T->p[P_DLY] = T->p[P_REV] = 0; } while (0)
    GR_FRESH(SHD_M_ONE);
    d0 = sg_run(FS * 2u, 0, 60);
    h0 = sg_hz(FS / 10u, FS * 3u / 10u);
    GR_FRESH(SHD_M_ONE);
    T->p[P_E3] = 12;
    d1 = sg_run(FS * 2u, 0, 60);
    h1 = sg_hz(FS / 20u, FS * 2u / 10u);
    check("GRAIN OFF: PTCH +12 an octave up, half as long (varispeed)", h1 > h0 * 1.95 && h1 < h0 * 2.05 &&
          d1 * 2u < d0 + 2u * CTL && d1 * 2u + 2u * CTL > d0, "%.1f -> %.1f Hz, %u -> %u", h0, h1, d0, d1);
    GR_FRESH(SHD_M_ONE);
    sg_all(3, SHD_GM_STRETCH + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0);
    d2 = sg_run(FS * 2u, 0, 60);
    h2 = sg_hz(FS / 10u, FS * 3u / 10u);
    GR_FRESH(SHD_M_ONE);
    sg_all(3, SHD_GM_STRETCH + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0);
    T->p[P_E3] = 12;
    d3 = sg_run(FS * 2u, 0, 60);
    h3 = sg_tone(FS / 10u, FS * 4u / 10u, 150, 700);
    check("STRETCH: the slice's pitch as it is, as long (+ half a grain)", fabs(h2 - h0) < 2.0 && d2 >= d0 &&
          d2 <= d0 + shd_glen(SHD_GSIZE_DEF) / 2u + 2u * CTL, "%.1f Hz, %u for %u", h2, d2, d0);
    check("STRETCH: PTCH +12 an octave up, the length the same", h3 > h0 * 1.97 && h3 < h0 * 2.03 &&
          d3 + 2u * CTL >= d2 && d3 <= d2 + 2u * CTL, "%.1f Hz, %u for %u", h3, d3, d2);
    T->p[P_E3] = -12;
    GR_FRESH(SHD_M_ONE);
    sg_all(3, SHD_GM_STRETCH + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0);
    T->p[P_E3] = -12;
    d3 = sg_run(FS * 2u, 0, 60);
    h3 = sg_tone(FS / 10u, FS * 4u / 10u, 60, 400);
    check("STRETCH: PTCH -12 an octave down, the length the same", h3 > h0 * 0.48 && h3 < h0 * 0.52 &&
          d3 + 2u * CTL >= d2 && d3 <= d2 + 2u * CTL, "%.1f Hz, %u for %u", h3, d3, d2);
    /* the attack whole: BREAK's first slice (a kick) as grains = as played (DENS 2, no jitter, PTCH 0) */
    fresh(1, SHD_M_ONE, 0);
    sg_run(FS, 0, 60);
    memcpy(ref, sg_buf, sizeof ref);
    fresh(1, SHD_M_ONE, 0);
    sg_all(0, SHD_GM_STRETCH + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0);
    sg_run(FS, 0, 60);
    for (c = e0 = e1 = 0, i = 0; i < FS / 4u; i++) {
        c += (double)ref[i] * sg_buf[i];
        e0 += (double)ref[i] * ref[i];
        e1 += (double)sg_buf[i] * sg_buf[i];
    }
    c /= sqrt(e0 * e1 + 1);
    check("STRETCH: a slice as grains (PTCH 0) is the slice: its attack whole", c > 0.995 &&
          abs(sg_buf[3]) * 10 > abs(ref[3]) * 9 - 50, "correlation %.4f", c);
    /* PLAY at half the tempo: varispeed's slices end and leave gaps; STRETCH fills them, at pitch */
    for (k = 0; k < 2u; k++) {
        GR_FRESH(SHD_M_PLAY);
        T->p[P_SGATE] = 127;
        shd_kit[3].bus.bpm = 120;                          /* (1 s = 2 beats at 120) */
        song.g[G_BPM] = 60;
        if (k)
            shd_kit[3].bus.gmode = SHD_GM_STRETCH;         /* the track's GRAIN: the slices at TRACK */
        sg_run(FS * 3u, 0, 60);
        for (sil = 0, i = FS / 2u; i + 256u <= FS * 5u / 2u; i += 256u)
            sil += sg_peak(i, i + 256u) < 500;
        h0 = sg_tone(FS / 10u, FS * 4u / 10u, 150, 300);
        if (!k)
            check("PLAY at half the tempo, GRAIN OFF: gaps after each slice", sil > 60u, "%u quiet of 172", sil);
        else
            check("PLAY at half the tempo, the track's STRETCH: no gaps, the pitch kept", sil == 0u && h0 > 218 && h0 < 222,
                  "%u quiet, %.1f Hz", sil, h0);
    }
    song.g[G_BPM] = 120;
    /* FREEZE: GATE holds past the slice's end while held, ends after the release; ONE: as long again */
    GR_FRESH(SHD_M_GATE);
    sg_all(3, SHD_GM_FREEZE + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 20);
    d0 = sg_run(FS * 3u, FS * 3u / 2u, 60);
    check("FREEZE (GATE): held past the slice's end, at its pitch", sg_peak(FS * 5u / 4u, FS * 11u / 8u) > 4000 &&
          fabs(sg_tone(FS, FS * 11u / 8u, 150, 300) - 220) < 3, "%d, %.1f Hz", sg_peak(FS * 5u / 4u, FS * 11u / 8u),
          sg_tone(FS, FS * 11u / 8u, 150, 300));
    check("FREEZE (GATE): ends after the release", d0 > FS * 3u / 2u && d0 < FS * 5u / 2u, "%u", d0);
    GR_FRESH(SHD_M_ONE);
    sg_all(3, SHD_GM_FREEZE + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0);
    d0 = sg_run(FS * 3u, 0, 60);
    check("FREEZE (ONE): the slice, then its end held as long again", d0 > FS * 9u / 10u && d0 < FS * 23u / 20u, "%u", d0);
    /* LOOP held goes round; REV, PING, CLOUD end and stay bounded */
    GR_FRESH(SHD_M_LOOP);
    sg_all(3, SHD_GM_STRETCH + 1u, SHD_GSIZE_DEF, SHD_GDENS_DEF, 0);
    d0 = sg_run(FS * 2u, FS * 3u / 2u, 60);
    check("STRETCH LOOP: round while held, no gap", sg_peak(FS / 2u - 2000u, FS / 2u + 2000u) > 8000 && d0 > FS * 3u / 2u &&
          d0 < FS * 2u, "%u", d0);
    for (k = 0; k < 4u; k++) {
        static const char *const W[] = {"REV", "PING", "CLOUD", "CLOUD REV"};
        char what[80];
        GR_FRESH(SHD_M_ONE);
        sg_all(3, k >= 2u ? SHD_GM_CLOUD + 1u : SHD_GM_STRETCH + 1u, 30, 127, 127);
        for (i = 0; i < SLC_AUTO; i++)
            shd_kit[3].sl[i].rev = k == 1u ? SHD_PING : k != 2u ? SHD_REV : SHD_FWD;
        T->p[P_E3] = 7;
        d0 = sg_run(FS * 3u, 0, 60);
        snprintf(what, sizeof what, "%s (small, dense, all jitter): audible, bounded, ends", W[k]);
        check(what, sg_peak(0, FS / 2u) > 3000 && sg_peak(0, FS * 3u) < 32767 && d0 < (k == 1u ? FS * 3u / 2u : FS * 3u / 4u),
              "%d, %u", sg_peak(0, FS * 3u), d0);
    }
    /* the load: 8 voices of small dense CLOUD; at most SHD_GPART grains, the seeking within SHD_GBUD */
    GR_FRESH(SHD_M_GATE);
    sg_all(3, SHD_GM_CLOUD + 1u, 0, 127, 127);
    for (i = 0; i < 8u; i++)
        trk_note_on(T, (uint8_t)(60 + i), 110);
    for (ok = 1, k = 0; k < 400u; k++) {
        uint32_t g = 0, v, j;
        blocks(1);
        for (v = 0; v < NVOICE; v++)
            if (T->v[v].active)
                for (j = 0; j < SHD_GN; j++)
                    g += shd_mem(T)->gv[v].g[j].winc != 0u;
        ok &= g <= SHD_GPART + SHD_GN;
    }
    check("8 voices of CLOUD: the grains held to the part's limit, bounded", ok && out_peak < 32767 && out_peak > 2000, "%d", out_peak);
    /* the kit before GRAIN (SHF2) read: GRAIN TRACK, the defaults; the track's GRAIN OFF */
    {
        static shd_rec2_t o;
        smp_user_hdr_t *h = (smp_user_hdr_t *)((uint8_t *)host_slots + 2u * SMP_USER_SIZE);
        memset(&o, 0, sizeof o);
        o.magic = SHD_REC2_MAGIC;
        o.len = slc_usr[2].len;
        o.crc = h->crc;
        for (i = 0; i < SLC_AUTO; i++) {
            shd_sl_default(&shd_kit[3].sl[i]);
            memcpy(o.sl[i], &shd_kit[3].sl[i], 16u);
        }
        o.sl[1][0] = (uint8_t)-5;                          /* slice 2's PITCH */
        {
            shd_bus_t db;
            shd_bus_default(&db);
            memcpy(o.bus, &db, sizeof o.bus);
            o.bus[20] = 0xAA;                              /* (SHF2's pad: anything) */
        }
        o.rcrc = st_crc32(&o, sizeof o - 4u);
        if (!slc_rec_room(2))
            check("SHF2: room for the kit in the slot", 0, 0);
        memset((uint8_t *)h + SLC_REC_OFF, 0xFF, 0x1000u);
        memcpy((uint8_t *)h + SHD_REC_OFF, &o, sizeof o);
        smp_user_scan(2);
        check("a kit saved before GRAIN (SHF2) reads: its settings, GRAIN TRACK, the track's OFF",
              shd_kit[3].sl[1].pitch == -5 && shd_kit[3].sl[1].grain == SHD_GT_TRACK &&
              shd_kit[3].sl[1].gsize == SHD_GSIZE_DEF && shd_kit[3].bus.gmode == SHD_GM_OFF &&
              shd_kit[3].bus.gdens == SHD_GDENS_DEF, "%d %u %u", shd_kit[3].sl[1].pitch, shd_kit[3].sl[1].grain, shd_kit[3].bus.gmode);
    }
    memset((uint8_t *)host_slots + 2u * SMP_USER_SIZE, 0, SMP_USER_SIZE);
    smp_user_scan(2);
    song.g[G_BPM] = 120;
}


/* 11: FDBK: a channel's shaper fed back into itself. Off: as before; low: rings and dies; high: howls on after the
 * voices, bounded; back to 0: quiet again. ARP's FEEDBACK SWELL (shd_swell) does the same at any FDBK */
static void fb_tests(void)
{
    uint32_t k, i;
    double e_tail[4];
    static const uint8_t AMT[4] = {0, 30, 100, 0};
    for (k = 0; k < 4u; k++) {
        fresh(1, SHD_M_ONE, 5);
        T->p[P_E6] = 127;
#if FELUCCA_MANGLER                                    /* DECK MODE: track 1's deck, its MID band */
        T->p[P_DK_MTYPE] = SHD_D_HARD;
        T->p[P_DK_MDRV] = 60;
        T->p[P_DK_FBMID] = AMT[k];
        (void)i;
#else
        for (i = 0; i < SLC_AUTO; i++)
            shd_kit[0].sl[i].route = k == 3u ? SHD_DIRECT : SHD_MID;   /* (SWELL: the kit as it comes, DIRECT) */
        shd_kit[0].bus.type[1] = SHD_D_HARD;
        shd_kit[0].bus.drive[1] = 60;
        shd_kit[0].bus.fb[1] = AMT[k];
#endif
        shd_swell = k == 3u ? 100u : 0u;
        trk_note_on(T, 60, 110);
        blocks(FS / 4u / CTL);
        trk_note_off(T, 60);
        voices_off();                                  /* the voices gone: what is left is the bus */
        blocks(2u * FS / CTL);                         /* two seconds on */
        out_peak = 0;
        out_energy = 0;
        blocks(FS / 2u / CTL);
        e_tail[k] = out_energy / (FS / 2u);
        if (k >= 2u)
            check(k == 2u ? "FDBK 100: the MID channel howls on after the voices, bounded" :
                            "FEEDBACK SWELL (ARP): DIRECT slices howl through FULL at FDBK 0, bounded",
                  e_tail[k] > 1e6 && out_peak < 32767, "rms %.0f, peak %d", sqrt(e_tail[k]), out_peak);
        if (k == 2u) {
#if FELUCCA_MANGLER
            T->p[P_DK_FBMID] = 0;
#else
            shd_kit[0].bus.fb[1] = 0;
#endif
            blocks(FS / 2u / CTL);
            out_peak = 0;
            blocks(FS / 4u / CTL);
            check("FDBK back to 0: the howl stops", out_peak < 50, "peak %d", out_peak);
        }
    }
    shd_swell = 0;
    check("FDBK 0 and 30: nothing left two seconds after the voices", e_tail[0] < 1.0 && e_tail[1] < 1.0,
          "%.2f, %.2f", e_tail[0], e_tail[1]);
}


/* 12: DETECT. A legato line in USR3 (22.05 kHz; a saw-ish tone): A2, C3 crossfading through a deep dip (0.6 s), E3
 * through a shallow one (1.2 s), a bend up to G3 (1.8 .. 2.2 s), G3, noise (2.8 .. 3.2 s), D3 struck after it. NOTES:
 * the two note changes, nothing in the bend or the noise, each on a rising zero crossing; more with SENS; HITS: what
 * pass 2 finds (D3's strike); BOTH: all of those. And what it costs */
static int16_t dt_y[22050 * 4];
static int dt_near(uint32_t at)
{
    uint32_t i;
    for (i = 1; i < slc_usr[2].nauto; i++)
        if (slc_usr[2].apos[i] + 551u >= at && slc_usr[2].apos[i] <= at + 551u)   /* (25 ms) */
            return 1;
    return 0;
}
static uint32_t dt_in(uint32_t a, uint32_t b)
{
    uint32_t i, n = 0;
    for (i = 1; i < slc_usr[2].nauto; i++)
        n += slc_usr[2].apos[i] > a && slc_usr[2].apos[i] < b;
    return n;
}
static void det_tests(void)
{
    const uint32_t FR = 22050;
    uint32_t i, n0, n64, n127, zc = 1;
    double ph = 0;
    int32_t ns = 12345;
    clock_t t0;
    double th, tb;
    for (i = 0; i < NELEM(dt_y); i++) {
        double t = (double)i / FR, f, a = 0.7, x;
        if (t < 0.6) f = 110;
        else if (t < 1.2) f = 130.81;
        else if (t < 1.8) f = 164.81;
        else if (t < 2.2) f = 164.81 * pow(2, (t - 1.8) / 0.4 * 3 / 12.0);
        else if (t < 2.8) f = 196;
        else if (t < 3.2) f = 0;
        else f = 146.83;
        if (fabs(t - 0.6) < 0.05) a *= 0.45 + 0.55 * fabs(t - 0.6) / 0.05;   /* the deep dip */
        if (fabs(t - 1.2) < 0.05) a *= 0.8 + 0.2 * fabs(t - 1.2) / 0.05;     /* the shallow one */
        if (t >= 3.2 && t < 3.205) a *= (t - 3.2) / 0.005;                    /* D3 struck */
        if (f > 0) {
            ph += f / FR;
            ph -= floor(ph);
            x = sin(2 * M_PI * ph) + 0.4 * sin(4 * M_PI * ph) + 0.2 * sin(6 * M_PI * ph);
        } else {
            ns = ns * 1103515245 + 12345;
            x = ((ns >> 16) & 0x7FFF) / 16384.0 - 1.0;
            a = 0.5 * exp(-(t - 2.8) / 0.2);
        }
        dt_y[i] = (int16_t)(x * a * 14000);
    }
    slot_pcm(2, dt_y, NELEM(dt_y));
    slc_rescan(2, 64, SLC_DET_NOTES);
    n64 = slc_usr[2].nauto;
    check("DETECT NOTES: the deep crossover (A2 to C3) and the shallow one (C3 to E3)", dt_near(FR * 6u / 10u) &&
          dt_near(FR * 12u / 10u), "%u slices", n64 - 1u);
    check("DETECT NOTES: nothing in the bend, at its ends or in the noise", !dt_in(FR * 17u / 10u, FR * 23u / 10u) &&
          !dt_in(FR * 28u / 10u, FR * 32u / 10u), "%u, %u", dt_in(FR * 17u / 10u, FR * 23u / 10u), dt_in(FR * 28u / 10u, FR * 32u / 10u));
    for (i = 1; i < slc_usr[2].nauto; i++) {
        uint32_t p = slc_usr[2].apos[i];
        if (!(p > 0 && dt_y[p - 1u] < 64 && dt_y[p] >= -64))
            printf("shard:   (slice at %u: %d %d %d)\n", p, dt_y[p - 1u], dt_y[p], dt_y[p + 1u]);
        zc &= p > 0 && dt_y[p - 1u] < 64 && dt_y[p] >= -64;   /* (ADPCM: within its error of the crossing) */
    }
    check("DETECT NOTES: each slice on a rising zero crossing", zc, 0);
    slc_rescan(2, 0, SLC_DET_NOTES);
    n0 = slc_usr[2].nauto;
    slc_rescan(2, 127, SLC_DET_NOTES);
    n127 = slc_usr[2].nauto;
    check("DETECT NOTES: SENS 0 finds fewer, 127 more", n0 <= n64 && n64 <= n127 && n0 >= 2u, "%u %u %u", n0 - 1u, n64 - 1u, n127 - 1u);
    t0 = clock();
    for (i = 0; i < 4u; i++)
        slc_rescan(2, 64, SLC_DET_HITS);
    th = (double)(clock() - t0) / 4;
    check("DETECT HITS: D3 struck after the noise", dt_near(FR * 32u / 10u), "%u slices", slc_usr[2].nauto - 1u);
    t0 = clock();
    for (i = 0; i < 4u; i++)
        slc_rescan(2, 64, SLC_DET_BOTH);
    tb = (double)(clock() - t0) / 4;
    check("DETECT BOTH: the crossovers and the strike", dt_near(FR * 6u / 10u) && dt_near(FR * 12u / 10u) &&
          dt_near(FR * 32u / 10u) && !dt_in(FR * 19u / 10u, FR * 21u / 10u), "%u slices", slc_usr[2].nauto - 1u);
    check("DETECT BOTH: the scan a few times HITS's at most", tb < th * 10 + 1, "x %.1f (host)", tb / (th + 1e-9));
    memset((uint8_t *)host_slots + 2u * SMP_USER_SIZE, 0, SMP_USER_SIZE);
    smp_user_scan(2);
}


/* 13: playing the loops. ARP's PITCH moves the howl an octave; the howl's ceiling; PANIC stops it and holds FDBK off
 * a while; PRESS: the keys' pressure takes FDBK 50 (rings, dies) past the edge */
static double fp_hz(void)
{
    int32_t o[2 * CTL], prev = 0;
    uint32_t f, i, z = 0;
    for (f = 0; f < FS / 4u; f += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < CTL; i++) {
            z += prev < 0 && o[2 * i] >= 0;
            prev = o[2 * i];
            out_peak = abs(o[2 * i]) > out_peak ? abs(o[2 * i]) : out_peak;
        }
    }
    return z * 4.0;
}
static void fp_setup(uint32_t fb)
{
    uint32_t i;
    fresh(1, SHD_M_ONE, 5);
    T->p[P_E6] = 127;
#if FELUCCA_MANGLER                                    /* DECK MODE: track 1's deck, its MID band */
    T->p[P_DK_MTYPE] = SHD_D_HARD;
    T->p[P_DK_MDRV] = 60;
    T->p[P_DK_FBMID] = (int16_t)fb;
    (void)i;
#else
    for (i = 0; i < SLC_AUTO; i++)
        shd_kit[0].sl[i].route = SHD_MID;
    shd_kit[0].bus.type[1] = SHD_D_HARD;
    shd_kit[0].bus.drive[1] = 60;
    shd_kit[0].bus.fb[1] = (uint8_t)fb;
#endif
}
static void fp_tests(void)
{
    double h0, h1;
    int32_t pk;
    fp_setup(100);
    trk_note_on(T, 60, 110);
    blocks(FS / 4u / CTL);
    trk_note_off(T, 60);
    voices_off();
    blocks(FS / CTL);
    out_peak = 0;
    h0 = fp_hz();
    pk = out_peak;
    shd_fbpitch = 192;                              /* (an octave up) */
    blocks(FS / 2u / CTL);
    h1 = fp_hz();
    shd_fbpitch = 0;
    check("ARP PITCH: the howl an octave up", h0 > 300 && h1 > h0 * 1.6 && h1 < h0 * 2.6, "%.0f -> %.0f Hz", h0, h1);
    check("the howl's ceiling: under -3 dBFS", pk > 4000 && pk < 23000, "peak %d", pk);
    shd_kill++;                                     /* PANIC */
    blocks(FS / 20u / CTL);
    out_peak = 0;
    blocks(FS / 4u / CTL);
    check("PANIC: the howl gone within 50 ms, and stays (-44 dB: the shapers' own tails)", out_peak < 200, "peak %d", out_peak);
    trk_note_on(T, 60, 110);                        /* (a hit while FDBK is held off: no howl after it) */
    blocks(FS / 8u / CTL);
    trk_note_off(T, 60);
    voices_off();
    blocks(FS / 8u / CTL);
    out_peak = 0;
    blocks(FS / 8u / CTL);
    check("PANIC: FDBK held off ~0.5 s (a hit then does not start it)", out_peak < 50, "peak %d", out_peak);
    blocks(FS / CTL);
    {
        uint32_t k;
        double e[2];
        for (k = 0; k < 2u; k++) {
            fp_setup(50);
#if FELUCCA_MANGLER
            T->p[P_DK_PRESS] = 127;
#else
            shd_kit[0].bus.press = 127;
#endif
            T->at = k ? 127 : 0;
            trk_note_on(T, 60, 110);
            blocks(FS / 4u / CTL);
            trk_note_off(T, 60);
            voices_off();
            blocks(2u * FS / CTL);
            out_energy = 0;
            blocks(FS / 2u / CTL);
            e[k] = out_energy / (FS / 2u);
            T->at = 0;
            shd_kit[0].bus.press = 0;
        }
        check("PRESS: FDBK 50 dies away untouched, howls pressed", e[0] < 1.0 && e[1] > 1e5, "%.1f, %.0f", e[0], e[1]);
    }
}


/* 14: ARP PRE. RING MID held, PRE: a 220 Hz slice comes out at 200 and 640 Hz (the carrier's sidebands) and the slice's
 * own low-pass (FILTER LP, CUT low) then takes the 640 off: the ring before the slice's effects. POST: the mix
 * (the filter before the ring, the 640 there) */
static void pre_tests(void)
{
    static int16_t y[22050];
    uint32_t i, k;
    double r[2];
    for (i = 0; i < NELEM(y); i++)
        y[i] = (int16_t)(sin(2 * M_PI * 220 * i / 22050.0) * 16000);
    slot_pcm(2, y, NELEM(y));
    for (k = 0; k < 2u; k++) {
        int32_t o[2 * CTL];
        uint32_t f;
        static int32_t buf[FS / 2];
        fresh(1, SHD_M_ONE, 0);
        T->p[P_E0] = 3;
        T->p[P_DLY] = T->p[P_REV] = 0;
        for (i = 0; i < SLC_AUTO; i++) {
            shd_kit[3].sl[i].filt = SHD_F_LP;
            shd_kit[3].sl[i].cut = 40;
            shd_kit[3].sl[i].res = 0;
        }
        arp_pre = (uint8_t)!k;
        perf_press(PF_RGM, 1);
        trk_note_on(T, 60, 110);
        for (f = 0; f + CTL <= FS / 2u; f += CTL) {
            mix_block(o, CTL);
            for (i = 0; i < CTL; i++)
                buf[f + i] = o[2 * i];
        }
        perf_press(PF_RGM, 0);
        blocks(FS / 10u / CTL);
        {
            double p200 = 0, p640 = 0, a, b;
            uint32_t j;
            for (j = 0; j < 2u; j++) {
                double fr = j ? 640 : 200, re = 0, im = 0;
                for (i = FS / 10u; i < FS / 2u; i++) {
                    re += buf[i] * cos(2 * M_PI * fr * i / FS);
                    im += buf[i] * sin(2 * M_PI * fr * i / FS);
                }
                a = re * re + im * im;
                if (j)
                    p640 = a;
                else
                    p200 = a;
            }
            b = p640 / (p200 + 1);
            r[k] = b;
        }
    }
    arp_pre = 0;
    check("ARP PRE: RING before the slice's FILTER (its 640 Hz cut), POST after it (640 Hz kept)", r[0] * 8 < r[1] && r[1] > 0.3,
          "640/200: PRE %.3f, POST %.3f", r[0], r[1]);
    memset((uint8_t *)host_slots + 2u * SMP_USER_SIZE, 0, SMP_USER_SIZE);
    smp_user_scan(2);
}


/* 16: SAMPLE's START, LEN and PLAY (eng_sample.c sx_*): the order of positions each mode reads, every value as a plain
 * decode of the zone gives, START moved while a loop plays, LEN 0 (a save of before) the whole zone, a tiny held loop
 * that sounds on past the zone's length, a far START waiting for the index (the projects' LEN 0 -> 127: proj_test) */
static int16_t smp_ref[65536];
static uint32_t smp_seq(track_t *t, voice_t *v, uint32_t *pos, int32_t *val, uint32_t want)
{
    const smp_zone_t *z = smp_zone((uint32_t)v->s[4]);
    uint32_t k = 0, guard = 0, zl = sx_zl((uint32_t)t->p[P_E0] % SMP_NALL, (uint32_t)v->s[4]);
    while (k < want && guard++ < 200000u) {
        int32_t x;
        if (v->ph[0] >= z->n) {
            pos[k] = 0xFFFFFFFFu;                       /* (the end of a one-shot) */
            return k + 1u;
        }
        pos[k] = v->ph[0];
        x = sx_next(sx_of(t), v, z, zl, t->p);
        if (x == SX_WAIT) {
            sample_block(t);
            continue;
        }
        val[k++] = x;
    }
    return k;
}
static void smp_tests(void)
{
    static uint32_t pos[4096];
    static int32_t val[4096];
    track_t *t = &trk[1];
    voice_t *v = &t->v[0];
    const smp_zone_t *z;
    uint32_t i, n, lo, hi, k, ok, bad = 0, m;
    int32_t pr = 0, ix = 0;
    voices_off();
    host_tracks_init();
    host_preset(t, ENGI_SAMPLE, 2);                     /* FLUTE: a built-in set, its zones looped */
    eng_mem_claim(t);
    t->p[P_E5] = 32;                                    /* START 25 % */
    t->p[P_E7] = 40;                                    /* LEN ~ 9 ms */
    v->note = 72;
    sample_note_on(t, v);
    z = smp_zone((uint32_t)v->s[4]);
    n = z->n;
    for (i = 0; i < n && i < 65536u; i++)
        smp_ref[i] = (int16_t)sx_dec(z, i, &pr, &ix);
    sx_region(t->p, z, &lo, &hi);
    check("SAMPLE: START 25 %, LEN 40: a region inside the zone", lo == n * 32u / 128u && hi > lo + 50u && hi < n,
          "%u..%u of %u", lo, hi, n);
    for (m = SX_ONE; m <= SX_RLOOP; m++) {
        uint32_t len = hi - lo, cnt;
        t->p[P_E3] = (int16_t)m;
        memset(v, 0, sizeof *v);
        v->note = 72;
        sample_note_on(t, v);
        cnt = smp_seq(t, v, pos, val, 3u * len + 3u);
        ok = 1;
        for (k = 0; k < cnt; k++) {
            uint32_t want;
            if (m == SX_ONE) want = k < len ? lo + k : 0xFFFFFFFFu;
            else if (m == SX_SMPL) want = lo + k;   /* (looped zones: on past LEN toward their own loop) */
            else if (m == SX_LOOP) want = lo + k % len;
            else if (m == SX_PONG) { uint32_t per = 2u * (len - 1u), q = k % per; want = q < len ? lo + q : hi - 1u - (q - len + 1u); }
            else if (m == SX_REV) want = k < len ? hi - 1u - k : 0xFFFFFFFFu;
            else want = hi - 1u - k % len;
            if (pos[k] != want || (want != 0xFFFFFFFFu && val[k] != smp_ref[want]))
                ok = 0;
            if (want == 0xFFFFFFFFu)
                break;
        }
        bad += !ok;
        printf("shard: SAMPLE PLAY %-5s: %u reads, %s\n", SX_PLAY_NAMES[m], cnt, ok ? "positions and values as a plain decode" : "WRONG");
    }
    check("SAMPLE PLAY: ONE SMPL LOOP PONG REV RLOOP read in their order, every value a plain decode's", bad == 0u, "%u modes wrong", bad);
    /* START moved while LOOP plays: the next pass starts at the new START (a wavetable scan) */
    t->p[P_E3] = SX_LOOP;
    memset(v, 0, sizeof *v);
    v->note = 72;
    sample_note_on(t, v);
    smp_seq(t, v, pos, val, (hi - lo) / 2u);
    t->p[P_E5] = 64;
    sx_region(t->p, z, &lo, &hi);
    k = smp_seq(t, v, pos, val, 3u * (hi - lo));
    for (i = 0, ok = 0; i < k; i++)
        ok |= pos[i] == lo;
    check("SAMPLE LOOP: START moved while it plays: the loop moves with it", ok && lo == n / 2u, "new START %u", lo);
    t->p[P_E7] = 0;                                     /* a save of before: the slot was unused */
    sx_region(t->p, z, &lo, &hi);
    check("SAMPLE LEN 0 (a project or preset of before): the rest of the zone", hi == n, "%u..%u of %u", lo, hi, n);
    {   /* a tiny held loop (LEN 5 ~ 1.4 ms, a wavetable) through the whole engine: it sounds on past the zone's length */
        double e0, e1;
        uint32_t b, zone_blocks = n / CTL + 1u;
        voices_off();
        host_tracks_init();
        host_preset(T, ENGI_SAMPLE, 2);
        T->p[P_E3] = SX_LOOP; T->p[P_E5] = 40; T->p[P_E7] = 5; T->p[P_SUS] = 127;
        blocks(8);
        trk_note_on(T, 60, 110);
        out_energy = 0; blocks(zone_blocks); e0 = out_energy;
        out_energy = 0; blocks(zone_blocks); e1 = out_energy;
        for (b = 0, k = 0; b < NVOICE; b++) k += T->v[b].active;
        check("SAMPLE LOOP LEN 5 (a single cycle, held): sounds on past the zone's length, as loud", k >= 1u && e1 > e0 * 0.7 && e0 > 0,
              "energy %.3g then %.3g, %u voice", e0, e1, k);
        trk_note_off(T, 60);
    }
    {   /* a far START right after the SET changed: the voice waits for its index entry, then sounds (within ~0.1 s) */
        uint32_t b, first = 0;
        voices_off();
        host_tracks_init();
        host_preset(T, ENGI_SAMPLE, 3);                 /* SAX: a fresh index */
        T->p[P_E3] = SX_ONE; T->p[P_E5] = 120; T->p[P_E7] = 127;
        blocks(1);
        trk_note_on(T, 60, 110);
        for (b = 1; b <= 400u && !first; b++) {   /* (the voice's first read: s[5] bit 1; the FX tails of the tests
                                                 * before would fool a level) */
            uint32_t q;
            blocks(1);
            for (q = 0; q < NVOICE; q++)
                if (T->v[q].active && T->v[q].note == 60 && (T->v[q].s[5] & 2))
                    first = b;
        }
        check("SAMPLE START 94 % on a fresh SET: waits for its index, then plays (within ~0.1 s)", first > 2u && first < 140u,
              "plays after %u blocks", first);
        trk_note_off(T, 60);
    }
    voices_off();
}

/* 15: udiv64 (core.h: the device has no 64-bit division) against the compiler's, on random and edge values */
static void udiv_tests(void)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    uint32_t i, bad = 0;
    static const uint64_t A[] = {0, 1, 0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull, 123456789012345ull};
    static const uint32_t B[] = {1, 2, 3, 7, 0xFFFFFFFFu, 65536u};
    for (i = 0; i < NELEM(A) * NELEM(B); i++)
        bad += udiv64(A[i / NELEM(B)], B[i % NELEM(B)]) != A[i / NELEM(B)] / B[i % NELEM(B)];
    for (i = 0; i < 200000u; i++) {
        uint64_t a;
        uint32_t b;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        a = x >> (x & 63u);
        b = (uint32_t)(x >> 32) >> ((x >> 8) & 31u);
        if (b)
            bad += udiv64(a, b) != a / b;
    }
    check("udiv64: as the compiler's 64-bit division (200k random, the edges)", bad == 0u && udiv64(5, 0) == 0u, "%u wrong", bad);
}

int main(int argc, char **argv)
{
    const char *loop = argc > 1 ? argv[1] : "build/slice_demo/loop";
    const char *dir = argc > 2 ? argv[2] : "build/shard_demo";
    char path[512];
    uint32_t i, at[64], sl[64], n, k, src;
    int ok;

    /* 1 */
#if FELUCCA_MANGLER
    check("the mangler: SHARD (15), SAMPLE, GRAIN offered, in that order", ENGINES[ENGI_SHARD] == &ENG_SHARD &&
          NENG_SHOWN == 3u + FELUCCA_ADPCM && eng_vis(0) == ENGI_SHARD && eng_vis(1) == ENGI_SAMPLE &&
          eng_vis(2) == ENGI_GRAIN, "%u shown", NENG_SHOWN);
    for (ok = 1, i = 0; i < NENGINES; i++)
        ok &= eng_ok(i) == (i == ENGI_SHARD || i == ENGI_SAMPLE || i == ENGI_GRAIN || (FELUCCA_ADPCM && i == ENGI_ADPCM)) &&
              (eng_ok(i) || ENGINES[i]->note_on != 0);
    check("the mangler: every synth engine's number reserved (never offered, silent)", ok, 0);
    for (ok = 1, i = 0; i < NPART; i++)
        ok &= eng_ok(trk_def_engine(i));
    check("the mangler: the power-on tracks play SHARD / GRAIN", ok, 0);
#else
    check("SHARD is engine 15, after DRUM in the shown order", ENGINES[ENGI_SHARD] == &ENG_SHARD &&
          eng_rank(ENGI_SHARD) == eng_rank(ENGI_DRUM) + 1u, 0);
    check("engine 14 reserved without ADPCM: never offered", FELUCCA_ADPCM || (!eng_ok(14) && NENG_SHOWN == 14u),
          "%u shown", NENG_SHOWN);
#endif
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
    {   /* (2026-11-14, from the hardware) LP fully open, RES low: the Chamberlin filter ran away (f q 2.16) and the
         * slice came out distorted. Now SHD_SVF_QMAX holds it: the slice as with FILTER OFF, at every low RES */
        double e[4];
        int32_t pk[4];
        uint32_t r, w = 1;
        static const uint8_t RES[3] = {0, 15, 30};
        fresh(1, SHD_M_LOOP, 5);                         /* (a run first: the tests before leave voices, a fade) */
        blocks(400);
        for (r = 0; r < 4u; r++) {
            fresh(1, SHD_M_LOOP, 5);
            for (i = 0; i < SLC_AUTO; i++) {
                shd_kit[0].sl[i].filt = r ? SHD_F_LP : SHD_F_OFF;
                shd_kit[0].sl[i].cut = 127;
                shd_kit[0].sl[i].res = r ? RES[r - 1u] : 0;
                shd_kit[0].sl[i].dtype = 0;              /* (the test before left DRIVE on) */
                shd_kit[0].sl[i].drv = 0;
            }
            trk_note_on(T, 60, 100);
            blocks(40);                                  /* (the voice under way) */
            out_peak = 0;
            out_energy = 0;
            blocks(200);
            e[r] = out_energy;
            pk[r] = out_peak;
        }
        for (r = 1; r < 4u; r++)
            w &= e[r] > e[0] * 0.8 && e[r] < e[0] * 1.25 && pk[r] < pk[0] * 5 / 4 + 200;
        printf("shard: LP CUT 127 (RES 0, 15, 30) against FILTER OFF: energy x%.2f x%.2f x%.2f, peak %d %d %d / %d\n",
               e[1] / e[0], e[2] / e[0], e[3] / e[0], (int)pk[1], (int)pk[2], (int)pk[3], (int)pk[0]);
        check("LP fully open, low RES: the slice as with FILTER OFF (it ran away and distorted)", w, 0);
    }
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
    shd_kit[src].sl[4].sus = 77;
    shd_kit[src].sl[4].rel = 33;
    shd_kit[src].sl[4].dly = 99;
    shd_kit[src].sl[4].rev = SHD_PING;
    shd_kit[src].bus.type[3] = SHD_D_FUZZ;
    shd_kit[src].bus.sens = 100;
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
        check("SUSTAIN, RELEASE, DELAY, DIR, FULL's type and SENS come back too", shd_kit[src].sl[4].sus == 77 &&
              shd_kit[src].sl[4].rel == 33 && shd_kit[src].sl[4].dly == 99 && shd_kit[src].sl[4].rev == SHD_PING &&
              shd_kit[src].bus.type[3] == SHD_D_FUZZ && shd_kit[src].bus.sens == 100, 0);
        {
            uint32_t na = slc_usr[0].nauto;
            slc_rescan(0, SHD_SENS_DEF, SLC_DET_HITS);
            check("SENS from the kit at load: its AUTO slices are SENS 100's, not the default's", na >= slc_usr[0].nauto,
                  "%u at 100, %u at 64", na, slc_usr[0].nauto);
            slc_rescan(0, 100, SLC_DET_HITS);
        }
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
                  j.sl[i].pitch >= -24 && j.sl[i].pitch <= 24 && j.sl[i].filt < SHD_NFILT && j.sl[i].choke <= 4u &&
                  j.sl[i].rev < SHD_NDIR && j.sl[i].sus <= 127u && j.sl[i].rel <= 127u && j.sl[i].dly <= 127u;
        for (i = 0; i < SHD_NCH; i++)
            ok &= j.bus.type[i] < SHD_NDRIVE && j.bus.level[i] <= 127u;
        ok &= j.bus.sens <= 127u;
        check("a kit of junk reads in range", ok, 0);
    }

    /* 9: the diagram's controls: SUSTAIN, RELEASE, DIR PING, COMB-/+, FUZZ, DELAY's send, FULL, SENS */
    {
        static int32_t buf[FS * 2];
        double e0, e1;
        uint32_t nb0, nb1, d, k;
        /* SUSTAIN: a fast DECAY falls to SUSTAIN and holds there (LOOP, held); 0: to silence */
        for (k = 0; k < 2u; k++) {
            fresh(1, SHD_M_LOOP, SHD_DIV_AUTO);
            for (i = 0; i < SLC_AUTO; i++) {
                shd_kit[0].sl[i].dec = 30;
                shd_kit[0].sl[i].sus = k ? 90 : 0;
            }
            trk_note_on(T, 60, 110);
            blocks(FS / 2u / CTL);
            out_energy = 0;
            blocks(FS / 10u / CTL);
            if (k) e1 = out_energy; else e0 = out_energy;
            voices_off();
        }
        check("SUSTAIN: DECAY stops at it and holds (LOOP); at 0 it decays to silence", e0 < 1e3 && e1 > 1e9,
              "%.3g, %.3g", e0, e1);
        /* RELEASE: from the key's release, faster than the track's own (GATE) */
        for (k = 0; k < 2u; k++) {
            fresh(1, SHD_M_GATE, SHD_DIV_AUTO);
            T->p[P_REL] = 90;                                    /* a long release on the track */
            for (i = 0; i < SLC_AUTO; i++)
                shd_kit[0].sl[i].rel = k ? 10 : 0;
            trk_note_on(T, 60, 110);
            blocks(FS / 50u / CTL);
            trk_note_off(T, 60);
            blocks(FS / 50u / CTL);
            out_energy = 0;
            blocks(FS / 10u / CTL);
            if (k) e1 = out_energy; else e0 = out_energy;
            voices_off();
        }
        check("RELEASE: a short one ends the slice before the track's long release", e1 < e0 * 0.05 && e0 > 0,
              "%.3g against %.3g", e1, e0);
        /* DIR PING: once, forward and back (ONE: twice as long); LOOP: to and fro while held */
        for (k = 0; k < 2u; k++) {
            fresh(1, SHD_M_ONE, 3);                            /* DIV 8: long slices */
            for (i = 0; i < SLC_AUTO; i++)
                shd_kit[0].sl[i].rev = k ? SHD_PING : SHD_FWD;
            trk_note_on(T, 60, 110);
            blocks(1);
            trk_note_off(T, 60);
            for (d = 1; nvoices() && d < 4000u; d++)
                blocks(1);
            if (k) nb1 = d; else nb0 = d;
        }
        check("DIR PING (ONE): forward then back, twice the slice", nb1 >= nb0 * 19u / 10u && nb1 <= nb0 * 21u / 10u,
              "%u blocks against %u", nb1, nb0);
        fresh(1, SHD_M_LOOP, 3);
        for (i = 0; i < SLC_AUTO; i++)
            shd_kit[0].sl[i].rev = SHD_PING;
        trk_note_on(T, 60, 110);
        blocks(nb0 * 5u);
        out_energy = 0;
        blocks(nb0 / 2u);
        check("DIR PING (LOOP): to and fro while held", nvoices() == 1u && out_energy > 1e9, "%u voices", nvoices());
        {   /* and it plays the slice backwards on the way back: the second half mirrors the first */
            fresh(1, SHD_M_ONE, 3);
            for (i = 0; i < SLC_AUTO; i++) {
                shd_kit[0].sl[i].rev = SHD_PING;
                shd_kit[0].sl[i].level = 100;
            }
            T->p[P_ATK] = 0;
            trk_note_on(T, 60, 110);
            for (d = 0; d < nb1 * CTL && d + CTL <= NELEM(buf); d += CTL) {
                int32_t o[2 * CTL];
                mix_block(o, CTL);
                for (i = 0; i < CTL; i++)
                    buf[d + i] = o[2 * i];
            }
            {
                uint32_t half = nb0 * CTL, a0 = half / 4u, cnt = half / 4u;
                double sab = 0, saa = 0, sbb = 0;
                uint32_t lag, best = 0;
                double bestc = -2;
                /* where the way back lines up with the way out: within a few blocks of the turn, mirrored */
                for (lag = 0; lag < 3u * CTL; lag += 2u) {
                    sab = saa = sbb = 0;
                    for (i = 0; i < cnt; i++) {
                        int32_t a = buf[a0 + i], b = buf[(2u * half + lag - a0 - i) % NELEM(buf)];
                        sab += (double)a * b; saa += (double)a * a; sbb += (double)b * b;
                    }
                    if (sab / sqrt(saa * sbb + 1) > bestc) { bestc = sab / sqrt(saa * sbb + 1); best = lag; }
                }
                check("DIR PING: the way back is the way out reversed", bestc > 0.9, "correlation %.2f (lag %u)", bestc,
                      best);
            }
            voices_off();
        }
        /* COMB+ / COMB-: the output correlates with itself at the comb's delay (+ / -) */
        {
            double c[3];
            for (k = 0; k < 3u; k++) {
                fresh(1, SHD_M_LOOP, SHD_DIV_AUTO);
                for (i = 0; i < SLC_AUTO; i++) {
                    shd_kit[0].sl[i].filt = (uint8_t)(k == 0 ? SHD_F_OFF : k == 1 ? SHD_F_COMBP : SHD_F_COMBN);
                    shd_kit[0].sl[i].cut = 70;
                    shd_kit[0].sl[i].res = 120;
                }
                trk_note_on(T, 62, 110);                       /* a snare: noisy */
                for (d = 0; d + CTL <= FS / 2u; d += CTL) {
                    int32_t o[2 * CTL];
                    mix_block(o, CTL);
                    for (i = 0; i < CTL; i++)
                        buf[d + i] = o[2 * i];
                }
                {
                    uint32_t lag = SHD_COMB_D[70] >> 8, n2 = FS / 2u - 2u * CTL - lag;
                    double sab = 0, saa = 0;
                    for (i = 0; i < n2; i++) {
                        sab += (double)buf[i] * (buf[i + lag] + buf[i + lag + 1]) / 2;
                        saa += (double)buf[i] * buf[i];
                    }
                    c[k] = sab / (saa + 1);
                }
                voices_off();
            }
            check("COMB+: a peak at its delay (CUT); COMB-: a trough", c[1] > c[0] + 0.3 && c[2] < c[0] - 0.3,
                  "off %.2f, + %.2f, - %.2f", c[0], c[1], c[2]);
        }
        /* FUZZ (the Fuzz War model): squashed flat (a crest factor well under clean's; its tone stack keeps it off a
         * pure square) */
        {
            double cf[2];
            for (k = 0; k < 2u; k++) {
                fresh(1, SHD_M_LOOP, SHD_DIV_AUTO);
                for (i = 0; i < SLC_AUTO; i++) {
                    shd_kit[0].sl[i].dtype = (uint8_t)(k ? SHD_D_FUZZ : SHD_D_OFF);
                    shd_kit[0].sl[i].drv = 127;
                }
                trk_note_on(T, 60, 110);
                blocks(FS / 20u / CTL);
                out_peak = 0;
                out_energy = 0;
                blocks(FS / 10u / CTL);
                cf[k] = out_peak / sqrt(out_energy / (FS / 10u / CTL * CTL) + 1);
                voices_off();
            }
            check("FUZZ: squashed (crest factor well under clean's)", cf[1] < 1.7 && cf[1] < cf[0] * 0.8, "fuzz %.2f, clean %.2f",
                  cf[1], cf[0]);
        }
        /* the analog models, shaper by shaper: OVRDRV (a triode) adds even harmonics, the old symmetric curve none;
         * DIST (a RAT) distorts 100 Hz less than 2 kHz (its legs to ground); FUZZ's sputter drops a quiet input */
        {
            shd_ds_t st;
            double h[3], lo, hi;
            uint32_t m;
            for (m = 1; m <= 2u; m++) {                     /* the 2nd harmonic against the 1st of 172 Hz */
                double re = 0, im = 0;
                memset(&st, 0, sizeof st);
                for (i = 0; i < 8192u; i++) {
                    int32_t y = shd_shape(SHD_D_SOFT, (int32_t)(16000 * sin(2 * M_PI * 172.27 * i / FS)), 90, &st);
                    if (i >= 4096u) {
                        re += y * cos(2 * M_PI * 172.27 * m * i / FS);
                        im += y * sin(2 * M_PI * 172.27 * m * i / FS);
                    }
                }
                h[m] = sqrt(re * re + im * im);
            }
            check("OVRDRV (triode): even harmonics", h[2] > h[1] * 0.05, "H2/H1 %.3f", h[2] / h[1]);
            for (m = 0; m < 2u; m++) {                      /* RAT: how much the drive squares 100 Hz vs 2 kHz */
                double f = m ? 2000.0 : 100.0, pk = 0, en = 0;
                memset(&st, 0, sizeof st);
                for (i = 0; i < 16384u; i++) {
                    int32_t y = shd_shape(SHD_D_HARD, (int32_t)(1200 * sin(2 * M_PI * f * i / FS)), 50, &st);
                    if (i >= 8192u) {
                        pk = fabs((double)y) > pk ? fabs((double)y) : pk;
                        en += (double)y * y;
                    }
                }
                *(m ? &hi : &lo) = pk / sqrt(en / 8192 + 1);
            }
            check("DIST (RAT): the lows stay cleaner than the highs", lo > hi + 0.1, "crest 100 Hz %.2f, 2 kHz %.2f", lo, hi);
            {
                int32_t mx = 0;
                memset(&st, 0, sizeof st);
                for (i = 0; i < 8192u; i++) {
                    int32_t y = shd_shape(SHD_D_FUZZ, (int32_t)(900 * sin(2 * M_PI * 200 * i / FS)), 127, &st);
                    if (i >= 4096u && (y < 0 ? -y : y) > mx)
                        mx = y < 0 ? -y : y;
                }
                check("FUZZ (Fuzz War): a quiet input sputters out at full drive", mx < 300, "peak %d", (int)mx);
            }
        }
        /* DELAY: the slice's send echoes with the track's own send at 0 */
        for (k = 0; k < 2u; k++) {
            fresh(1, SHD_M_ONE, SHD_DIV_AUTO);
            song.g[G_DMIX] = 100;
            song.g[G_DFDBK] = 40;
            for (i = 0; i < SLC_AUTO; i++) {
                shd_kit[0].sl[i].dly = (uint8_t)(k ? 127 : 0);
                shd_kit[0].sl[i].dec = 40;                     /* a short slice */
            }
            trk_note_on(T, 60, 110);
            blocks(1);
            trk_note_off(T, 60);
            for (d = 0; nvoices() && d < 4000u; d++)
                blocks(1);
            blocks(FS / 20u / CTL);
            out_energy = 0;
            blocks(FS * 3u / 2u / CTL);
            if (k) e1 = out_energy; else e0 = out_energy;
        }
        check("DELAY: the slice's send echoes after it ends; at 0, next to nothing (the delay line's leftovers)",
              e1 > 1e9 && e0 < e1 / 100, "%.3g against %.3g", e1, e0);
        song.g[G_DMIX] = 0;
        /* FULL: a channel of its own; FUZZ on it squares the slice, OFF leaves it as DIRECT (but for the bus's DC
         * blocker and gain ramp) */
        {
            double ed, ef;
            for (k = 0; k < 2u; k++) {
                fresh(1, SHD_M_LOOP, SHD_DIV_AUTO);
                T->p[P_E6] = 127;
                for (i = 0; i < SLC_AUTO; i++)
                    shd_kit[0].sl[i].route = (uint8_t)(k ? SHD_FULL : SHD_DIRECT);
                shd_kit[0].bus.type[3] = SHD_D_OFF;
                shd_kit[0].bus.level[3] = 127;
                trk_note_on(T, 60, 110);
                blocks(FS / 20u / CTL);
                out_energy = 0;
                blocks(FS / 5u / CTL);
                if (k) ef = out_energy; else ed = out_energy;
                voices_off();
            }
            check("FULL, clean: about as loud as DIRECT", ef > ed * 0.8 && ef < ed * 1.25, "%.3g against %.3g", ef, ed);
        }
    }
#if FELUCCA_MANGLER
    {   /* USR4 (the mangler): flash 0xE7000, SHARD's SRC 5 (after PIANO), its kit and tempo; SAMPLE / GRAIN's SET 8 */
        static double x[22050 * 4];
        static int16_t y[22050 * 4];
        uint32_t m = make_loop(x, NELEM(x), 128, "XHHHXHHHXHHHXHHH", 2, 0);
        uint8_t rec[0x1000];
        slot_pcm(3, y, to_pcm(x, y, m));
        check("USR4: at 0xE7000, below the globals, SRC 5, a slot of its own", smp_user_off(3) == 0xE7000u &&
              smp_user_off(2) == 0xC8000u && slc_src_of_slot(3) == 5u && slc_slot_of_src(5) == 3u &&
              slc_slot_of_src(4) >= SMP_USER_SLOTS && slc_get(5) == &slc_usr[3] && !strcmp(N_SLC_SRC[5], "USR4") &&
              !strcmp(SMP_ALL_NAMES[SMP_NSETS + 3], "USR4"), 0);
        check("USR4: scanned, its tempo found (2 bars at 128)", slc_usr[3].nauto > 4u && shd_det_bpm10[5] >= 1275u &&
              shd_det_bpm10[5] <= 1285u, "%u slices, %u", slc_usr[3].nauto, shd_det_bpm10[5]);
        fresh(0, SHD_M_ONE, SHD_DIV_AUTO);
        T->p[P_E0] = 5;
        trk_note_on(T, 60, 110);
        out_peak = 0;
        blocks(50);
        check("USR4: SHARD plays it", out_peak > 2000, "peak %d", out_peak);
        voices_off();
        /* the update loader's record test (ldr_core.c): a sector's 0xF00.. never reads as one in written sample data */
        memset(rec, 0x41, sizeof rec);
        smp_rec_guard(0xE7000u, rec, sizeof rec);
        ok = rec[0xF06] == 0x41u;                                /* "AA..": no record (no "AT", no CRC): untouched */
        rec[0xF06] = 0x41; rec[0xF07] = 0x54;
        {
            uint32_t c = smp_crc16(rec + 0xF02, 78);
            rec[0xF00] = (uint8_t)c; rec[0xF01] = (uint8_t)(c >> 8);
            ok &= c != 0u;
        }
        smp_rec_guard(0xE7000u, rec, sizeof rec);
        ok &= rec[0xF06] == 0x40u;
        check("USR4: data that would read as an update record is changed by a bit; other data is not", ok, 0);
        memset((uint8_t *)host_slots + 3u * SMP_USER_SIZE, 0, SMP_USER_SIZE);
        smp_user_scan(3);
    }
#endif
#if FELUCCA_MANGLER
    {   /* BOUNCE (bounce.c): the sequence into a slot */
        static int32_t live[FS * 4];
        uint32_t n22, f, i2;
        int32_t o[2 * CTL], prev = 0, odd = 0;
        double sab = 0, saa = 0, sbb = 0;
        slc_dec_t d;
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);                    /* track 1: SHARD RECYCLE playing BREAK's slices */
        for (i = 0; i < NSTEP; i++)
            T->step[i].time = ST_REST;
        for (i = 0; i < 16u; i++) {
            uint8_t nn = PATTERNS[10].note[i];
            put_step(T, i, 1u, &nn, ST_NOTE, 0u);
        }
        T->p[P_SLEN] = 16;
        song.g[G_BPM] = 120;
        for (i = 1; i < NTRK; i++)
            trk[i].p[P_MUTE] = 1;
        trk[2].p[P_MUTE] = 0;                                  /* (a track left on, but empty) */
        bnc.bars = 1;                                          /* 2 bars */
        bnc.from = 0;
        bnc.to = 1;                                            /* USR2 */
        n22 = bnc_len();
        check("BOUNCE: 2 bars at 120 BPM is 4 s (88,200 samples at 22,050 Hz), within the slot", n22 == 88200u &&
              bnc_want() == n22 && !bnc_why(), "%u", n22);
        ok = bnc_go() == 0;
        check("BOUNCE: done, said, the transport stopped, the audio back, the mutes as they were",
              ok && !strcmp(host_said, "BOUNCED TO USR2") && !song.playing && !bnc_on && trk[1].p[P_MUTE] == 1 &&
              trk[2].p[P_MUTE] == 0 && !trk[0].p[P_MUTE], "%s", host_said);
        check("BOUNCE: the slot holds it (one zone, 22,050 Hz, BOUNCE2) and SHARD sliced it", usr_nz[1] == 1u &&
              slc_usr[1].len == n22 && !memcmp(((const smp_user_hdr_t *)SMP_USER_XIP(1))->name, "BOUNCE2", 7) &&
              slc_usr[1].nauto > 8u, "%u samples, %u slices", slc_usr[1].len, slc_usr[1].nauto);
        check("BOUNCE: the kit's BPM is the song's, the tempo found agrees", shd_kit[2].bus.bpm == 120u &&
              shd_det_bpm10[2] >= 1195u && shd_det_bpm10[2] <= 1205u, "%u, %u", shd_kit[2].bus.bpm, shd_det_bpm10[2]);
        /* the same sequence played live, its second time through, decimated as the bounce does: the slot's sound */
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
        for (i = 0; i < NSTEP; i++)
            T->step[i].time = ST_REST;
        for (i = 0; i < 16u; i++) {
            uint8_t nn = PATTERNS[10].note[i];
            put_step(T, i, 1u, &nn, ST_NOTE, 0u);
        }
        T->p[P_SLEN] = 16;
        song.g[G_BPM] = 120;
        for (i = 1; i < NTRK; i++)
            trk[i].p[P_MUTE] = 1;
        song.master_q12 = MASTER_FULL;
        seq_start();
        for (f = 0; f < 2u * n22; f += CTL)
            mix_block(o, CTL);
        seq_stop();
        seq_start();
        for (f = 0; f < 2u * n22; f += CTL) {
            mix_block(o, CTL);
            for (i2 = 0; i2 < CTL; i2++) {
                int32_t m2 = (o[2u * i2] + o[2u * i2 + 1u]) >> 1;
                if ((f + i2) & 1u) {
                    live[(f + i2) >> 1] = (prev + 2 * odd + m2) >> 2;
                    prev = m2;
                } else
                    odd = m2;
            }
        }
        seq_stop();
        slc_dec_at(&d, 0, 0);
        for (f = 0; f < n22; f++) {
            int32_t x2 = slc_dec_next(&slc_usr[1], &d), y2 = clamp(live[f], -32768, 32767);
            sab += (double)x2 * y2; saa += (double)x2 * x2; sbb += (double)y2 * y2;
        }
        check("BOUNCE: the slot sounds as the sequence does (the second time through)", sab / sqrt(saa * sbb + 1) > 0.97 &&
              saa > 1e12, "correlation %.4f", sab / sqrt(saa * sbb + 1));
        /* what stops it */
        T->p[P_E0] = 2;                                        /* track 1 plays USR2 */
        bnc.from = 0;
        check("BOUNCE: not into a slot a track it renders plays", bnc_why() && !strcmp(bnc_why(), "USR2 PLAYS IN IT") &&
              bnc_go() == 1 && usr_nz[1] == 1u, "%s", bnc_why() ? bnc_why() : "-");
        T->p[P_MUTE] = 1;
        check("BOUNCE: a muted track does not count (MIX: the tracks not muted)", !bnc_why(), 0);
        T->p[P_MUTE] = 0;
        bnc.from = 3;
        check("BOUNCE: FROM T3 renders track 3 alone (track 1 on USR2 does not count)", !bnc_why(), 0);
        bnc.from = 0;
        T->p[P_E0] = 0;
        song.g[G_CLOCK] = 1;
        check("BOUNCE: the internal clock only", bnc_why() != 0, 0);
        song.g[G_CLOCK] = 0;
        /* longer than the slot: cut at 7.38 s, no BPM (not whole bars) */
        song.g[G_BPM] = 60;
        bnc.bars = 3;                                          /* 8 bars at 60: 32 s */
        bnc.to = 3;                                            /* USR4 */
        check("BOUNCE: 8 bars at 60 BPM: cut to what the slot holds beside its records", bnc_len() == BNC_MAX && BNC_MAX == 154624u && bnc_want() > bnc_len(),
              "%u of %u", bnc_len(), bnc_want());
        shd_kit[5].bus.bpm = 0;
        ok = bnc_go() == 0;
        check("BOUNCE: into USR4, cut: the slot full, no BPM set", ok && slc_usr[3].len == BNC_MAX && slc_rec_room(3) &&
              shd_kit[5].bus.bpm == 0u, "%u", slc_usr[3].len);
        song.g[G_BPM] = 120;
        for (i = 0; i < 2u; i++) {
            memset((uint8_t *)host_slots + (1u + 2u * i) * SMP_USER_SIZE, 0, SMP_USER_SIZE);
            smp_user_scan(1u + 2u * i);
        }
        for (i = 0; i < NTRK; i++)
            trk[i].p[P_MUTE] = 0;
    }
#endif
    {   /* SENS: more hits found as it rises, on a slot (USR3: hats, snares, kicks) */
        static double x[22050 * 4];
        static int16_t y[22050 * 4];
        uint32_t m = make_loop(x, NELEM(x), 120, "XHHHYHH.XHKHY.HH", 2, 0), n0, n64, n127;
        slot_pcm(2, y, to_pcm(x, y, m));
        slc_rescan(2, 0, SLC_DET_HITS);
        n0 = slc_usr[2].nauto;
        slc_rescan(2, 64, SLC_DET_HITS);
        n64 = slc_usr[2].nauto;
        slc_rescan(2, 127, SLC_DET_HITS);
        n127 = slc_usr[2].nauto;
        check("SENS: fewer slices at 0, more at 127", n0 <= n64 && n64 <= n127 && n0 < n127, "%u, %u, %u", n0, n64, n127);
        slc_rescan(2, SHD_SENS_DEF, SLC_DET_DEF);
        check("SENS 64 is SLICE's scan", slc_usr[2].nauto == n64, 0);
        memset((uint8_t *)host_slots + 2u * SMP_USER_SIZE, 0, SMP_USER_SIZE);
        smp_user_scan(2);
    }

    sg_tests();
    fb_tests();
    det_tests();
    fp_tests();
    pre_tests();
    udiv_tests();
    smp_tests();

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
        /* GRAIN: BREAK at 90 BPM, PTCH +7: STRETCH keeps the beat and moves the pitch; then CLOUD */
        fresh(0, SHD_M_PLAY, SHD_DIV_AUTO);
        T->p[P_SGATE] = 127;
        T->p[P_E3] = 7;
        shd_kit[0].bus.gmode = SHD_GM_STRETCH;
        demo(dir, "grain_stretch_break_90_up7.wav", 90, 4, SHD_M_PLAY, SHD_DIV_AUTO, 0, ONE, 16);
        shd_kit[0].bus.gmode = SHD_GM_CLOUD;
        shd_kit[0].bus.gjit = 70;
        shd_kit[0].bus.gdens = 100;
        demo(dir, "grain_cloud_break_90_up7.wav", 90, 4, SHD_M_PLAY, SHD_DIV_AUTO, 0, ONE, 16);
        /* FDBK: the kit routed through the bus, MID (DIST) fed back to its edge, LOW (TUBE) a little */
        fresh(1, SHD_M_ONE, 5);
        for (i = 0; i < SLC_AUTO; i++)
            shd_kit[0].sl[i].route = i % 3u ? SHD_MID : SHD_LOW;
        T->p[P_E6] = 127;
        shd_kit[0].bus.type[1] = SHD_D_HARD;
        shd_kit[0].bus.drive[1] = 70;
        shd_kit[0].bus.fb[1] = 72;
        shd_kit[0].bus.fb[0] = 50;
        shd_kit[0].bus.gate[1] = 30;
        demo(dir, "fdbk_kit_128.wav", 128, 4, SHD_M_ONE, 5, 0, CHOP, 16);
    }
    printf(fails ? "shard: %d FAILED\n" : "shard: all passed\n", fails);
    return fails != 0;
}

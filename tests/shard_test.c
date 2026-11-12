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

    /* 2 */
    fresh(1, SHD_M_ONE, SLC_DIV_AUTO);
    trk_note_on(T, 61, 110);                                   /* slice 1 of BREAK's AUTO slices */
    blocks(1);
    check("ONE: a note plays its slice", nvoices() == 1u && shd_slice_of(first_voice()) == 1u, 0);
    trk_note_off(T, 61);
    blocks(400);
    check("ONE: the slice plays to its end, then the voice ends", nvoices() == 0u && out_peak > 2000, "peak %d", out_peak);
    fresh(1, SHD_M_GATE, 2);
    trk_note_on(T, 60, 110);
    blocks(20);
    trk_note_off(T, 60);
    blocks(100);
    check("GATE: the release ends the voice", nvoices() == 0u, 0);
    fresh(1, SHD_M_LOOP, 3);                                   /* 32 slices: short ones */
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
        fresh(0, SHD_M_PLAY, SLC_DIV_AUTO);
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
        fresh(0, SHD_M_PLAY, SLC_DIV_AUTO);
        in_child(run_tempo, 120, a, RUN_N);
        in_child(run_tempo, 90, b, RUN_N);
        for (f = 0; f < upto; f++)
            diff += a[f] != b[f];
        check("PLAY: the tempo moves the slices, not the pitch (slice 1 alike)", diff == 0u, "%u of %u differ", diff, upto);
    }

    /* 4: per-slice settings */
    {
        double e0, e1;
        fresh(1, SHD_M_ONE, 2);
        trk_note_on(T, 60, 110);
        blocks(200);
        e0 = out_energy;
        trk_note_off(T, 60);
        fresh(1, SHD_M_ONE, 2);
        shd_kit[0].sl[0].level = 50;
        trk_note_on(T, 60, 110);
        blocks(200);
        e1 = out_energy;
        check("LEVEL 50 is quieter than 100 (about -6 dB)", e1 < e0 * 0.4 && e1 > e0 * 0.1, "%.2f", e1 / e0);
    }
    {
        uint32_t len[2];
        for (k = 0; k < 2u; k++) {
            fresh(1, SHD_M_ONE, 2);
            shd_kit[0].sl[0].pitch = k ? 12 : 0;
            trk_note_on(T, 60, 110);
            for (len[k] = 0; nvoices() && len[k] < 2000u; len[k]++)
                blocks(1);
        }
        check("PITCH +12 plays the slice in half the time", len[1] * 2u >= len[0] - 2u && len[1] * 2u <= len[0] + 2u,
              "%u / %u blocks", len[1], len[0]);
    }
    {
        fresh(1, SHD_M_ONE, 2);
        shd_kit[0].sl[0].dec = 40;
        trk_note_on(T, 60, 110);
        for (n = 0; nvoices() && n < 2000u; n++)
            blocks(1);
        check("DECAY 40 ends the slice early", n < 60u, "%u blocks", n);
    }
    {   /* REV: the voice reads backwards: its first source sample is the slice's last */
        voice_t *v;
        uint32_t a, b, st;
        fresh(1, SHD_M_ONE, 2);
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
            fresh(1, SHD_M_LOOP, 2);
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
        fresh(1, SHD_M_LOOP, 2);
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
        fresh(1, SHD_M_ONE, 2);
        in_child(run_route, SHD_DIRECT, a, RUN_N);
        in_child(run_route, SHD_MULTI, b, RUN_N);
        for (f = 0; f < RUN_N; f++)
            diff += a[f] != b[f];
        check("MULTI at MIX 0 == DIRECT, bit for bit", diff == 0u, "%u samples differ", diff);
    }
    for (ok = 1, k = SHD_MULTI; k < SHD_NROUTE; k++) {
        uint32_t ty;
        for (ty = 0; ty < SHD_NDRIVE; ty++) {
            fresh(1, SHD_M_LOOP, 2);
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
        fresh(0, SHD_M_PLAY, SLC_DIV_AUTO);
        T->p[P_SGATE] = 127;
        T->p[P_SLEN] = 16;
        demo(dir, "play_break_120.wav", 120, 4, SHD_M_PLAY, SLC_DIV_AUTO, 0, ONE, 16);
        demo(dir, "play_break_90.wav", 90, 4, SHD_M_PLAY, SLC_DIV_AUTO, 0, ONE, 16);
        fresh(1, SHD_M_ONE, 2);
        shd_kit[0].sl[1].rev = 1;
        shd_kit[0].sl[3].pitch = -5;
        shd_kit[0].sl[3].route = SHD_LOW;
        shd_kit[0].sl[4].filt = SHD_F_BP; shd_kit[0].sl[4].cut = 80; shd_kit[0].sl[4].res = 110;
        shd_kit[0].sl[5].route = SHD_MULTI;
        shd_kit[0].sl[6].dtype = SHD_D_FOLD; shd_kit[0].sl[6].drv = 90;
        shd_kit[0].sl[7].route = SHD_HIGH; shd_kit[0].sl[7].dec = 70;
        shd_kit[0].sl[8].pitch = 7; shd_kit[0].sl[8].route = SHD_MID;
        T->p[P_E7] = 20;
        demo(dir, "kit_break_routed.wav", 128, 4, SHD_M_ONE, 2, 0, CHOP, 16);
        fresh(0, SHD_M_PLAY, SLC_DIV_AUTO);
        T->p[P_SGATE] = 127;
        demo(dir, "play_usr_loop_110.wav", 110, 4, SHD_M_PLAY, SLC_DIV_AUTO, 1, ONE, 16);
    }
    printf(fails ? "shard: %d FAILED\n" : "shard: all passed\n", fails);
    return fails != 0;
}

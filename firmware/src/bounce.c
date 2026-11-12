/* SPDX-License-Identifier: GPL-3.0-only */
/* SAVE > BOUNCE (the sample mangler, FELUCCA_MANGLER): the sequence, rendered into a user sample slot.
 *
 * KNOB 1 BARS (1 2 4 8), KNOB 2 FROM (MIX: every track that is not muted; T1..T4: that track alone), KNOB 3 TO (USR1..
 * USR4), OCT+ BOUNCE. The transport stops; the audio goes quiet while the main loop renders: the sequence from its top
 * for BARS bars (unheard: it fills the delay and reverb and lets notes ring), then from the top again for BARS bars,
 * which are recorded: the tails of the end come round into the start, so the slot loops seamlessly. MASTER at its
 * top and no LOWCUT / BASS+ while it renders (the speaker's), the mix as it is otherwise (the FX buses, the FX layer).
 * The recording: mono ((L + R) / 2), 22,050 Hz (1 2 1 between pairs), IMA ADPCM as the web editor writes it, one zone
 * over the keyboard (root C4); 7.01 s at most (the slot's last sector stays for SHARD's records): a longer bounce
 * is cut there. Written as the editor's
 * upload is (eng_sample.c's layout: the header sector erased first, the data from +512 sector by sector, the header
 * with the data's CRC last), then scanned (SHARD's slices and tempo); SHARD's kit gets the song's tempo as its BPM
 * (the bounce is exactly BARS bars of it). It cannot write a slot one of the tracks it renders plays (the material
 * would change under it): "USR2 PLAYS IN IT". INT clock only (the render runs the sequencer itself).
 * A render runs at the CPU's pace (faster than real time); the screen shows how far it is (ui_draw, every ~100 ms). */
#if FELUCCA_MANGLER

#if FELUCCA_FLASH
static int fl_erase4k(uint32_t off, uint32_t *took_us);   /* fm1_flash.h, storage_hw.c (included after the UI) */
static int fl_write(uint32_t off, const uint8_t *src_ram, uint32_t n);
static void audio_silence(void);
#endif
static void ui_draw(void);                         /* ui_draw.c: the progress */

static const uint8_t BNC_BARS[4] = {1, 2, 4, 8};
static struct {
    uint8_t bars, from, to;                        /* BNC_BARS index; 0 MIX, 1..4 T1..T4; user slot */
    uint8_t busy;                                  /* 1 rendering the run-in, 2 recording, 3 writing the header */
    uint8_t pct;                                   /* how far, 0..100 */
    uint32_t last_ms;                              /* the last progress draw */
} bnc;

/* the samples (22,050 Hz) a bounce of the picked BARS takes at this tempo, and what the slot keeps of them */
static uint32_t bnc_want(void) { return BNC_BARS[bnc.bars & 3u] * 4u * beat_samples() / 2u; }
/* at most what leaves the slot's last sector to its slices' and kit's records (slice_store.c): 7.01 s */
#define BNC_MAX (2u * (SMP_USER_SIZE - 0x1000u - SMP_USER_DATA))
static uint32_t bnc_len(void) { uint32_t n = bnc_want(); return n < BNC_MAX ? n : BNC_MAX; }

/* the user slot track t plays, SMP_USER_SLOTS = none (a built-in source, another engine) */
static uint32_t bnc_slot_of(const track_t *t)
{
    uint32_t e = t->eng_req % NENGINES, si;
    if (e == ENGI_SHARD)
        return slc_slot_of_src(slc_src_of(t->p));
    if (e == ENGI_SAMPLE || e == ENGI_GRAIN) {
        si = (uint32_t)t->p[P_E0] % SMP_NALL;
        return si >= SMP_NSETS ? si - SMP_NSETS : (uint32_t)SMP_USER_SLOTS;
    }
    return SMP_USER_SLOTS;
}
static int bnc_renders(uint32_t i)                 /* track i sounds in the bounce */
{
    return bnc.from ? i == bnc.from - 1u : !trk[i].p[P_MUTE];
}
/* why a bounce cannot run now, 0 = it can */
static const char *bnc_why(void)
{
    static char msg[20];
    uint32_t i;
    if (song.g[G_CLOCK])
        return "INT CLOCK TO BOUNCE";
    for (i = 0; i < NTRK; i++)
        if (bnc_renders(i) && bnc_slot_of(&trk[i]) == bnc.to) {
            str_cpy(msg, SMP_ALL_NAMES[SMP_NSETS + bnc.to], sizeof msg);
            str_cpy(msg + str_len(msg), " PLAYS IN IT", sizeof msg - str_len(msg));
            return msg;
        }
    return 0;
}

/* ---- the slot: written as the editor's upload (editor.c ED_SMP_*) */
static uint32_t bnc_crc32_add(uint32_t c, const uint8_t *p, uint32_t n)   /* zlib's (as storage.c st_crc32_add) */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static int bnc_erase(uint32_t k, uint32_t rel)     /* slot k's sector at rel; 0 = done */
{
#if FELUCCA_FLASH
    uint32_t took;
    audio_silence();
    return fl_erase4k(smp_user_off(k) + rel, &took);
#else
    memset((uint8_t *)SMP_USER_XIP(k) + rel, 0xFF, 0x1000u);
    return 0;
#endif
}
static int bnc_prog(uint32_t k, uint32_t rel, uint8_t *b, uint32_t n)
{
    smp_rec_guard(smp_user_off(k) + rel, b, n);      /* (USR4: never an update record) */
#if FELUCCA_FLASH
    return fl_write(smp_user_off(k) + rel, b, n);
#else
    memcpy((uint8_t *)SMP_USER_XIP(k) + rel, b, n);
    return 0;
#endif
}

/* ---- the encoder: IMA ADPCM, as web/editor.html and tools/sampleio.py write it */
static const int16_t BNC_STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t BNC_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static struct {
    int32_t pred, idx;
    uint32_t n;                                    /* samples encoded */
    uint32_t at;                                   /* the page's slot offset */
    uint32_t crc;
    int32_t prev, odd;                             /* the decimator: the last 44.1 kHz sample, one waiting */
    uint8_t page[256];
    int err;
} be __attribute__((section(".pool")));          /* (main loop only: off the audio code's .bss) */
static uint32_t bnc_code(int32_t x)
{
    int32_t step = BNC_STEP[be.idx], diff = x - be.pred, vd = step >> 3;
    uint32_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) { code |= 4; diff -= step; vd += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; vd += step >> 1; }
    if (diff >= step >> 2) { code |= 1; vd += step >> 2; }
    be.pred = clamp(code & 8u ? be.pred - vd : be.pred + vd, -32768, 32767);
    be.idx = clamp(be.idx + BNC_IDX[code & 7u], 0, 88);
    return code;
}
static void bnc_flush(uint32_t k, uint32_t len)    /* the page (len bytes) to the slot, its sector erased first */
{
    if (!len || be.err)
        return;
    if (!(be.at & 0xFFFu) && bnc_erase(k, be.at))
        be.err = 2;
    else if (bnc_prog(k, be.at, be.page, len))
        be.err = 3;
    be.crc = bnc_crc32_add(be.crc, be.page, len);    /* (after the guard: the CRC of what the flash holds) */
    be.at += len;
}
static void bnc_put(uint32_t k, int32_t x)         /* one 22,050 Hz sample */
{
    uint32_t c = bnc_code(x), i = be.n >> 1 & 255u;
    if (be.n & 1u)
        be.page[i] |= (uint8_t)(c << 4);
    else
        be.page[i] = (uint8_t)c;
    be.n++;
    if (!(be.n & 511u))                            /* 512 samples: a page */
        bnc_flush(k, 256u);
}

/* ---- the render */
static void bnc_progress(uint32_t busy, uint32_t pct)
{
    bnc.busy = (uint8_t)busy;
    bnc.pct = (uint8_t)pct;
    if (fm1_ms - bnc.last_ms >= 100u) {            /* the screen: how far (main loop: the render is in it) */
        bnc.last_ms = fm1_ms;
        ui.force = 1;
        ui_draw();
    }
    fm1_wdt_feed();
}
static void bnc_render(uint32_t k, uint32_t n44, int rec)
{
    int32_t o[2 * CTL];
    uint32_t f, i, total = 2u * n44;
    for (f = 0; f < n44 && !be.err; f += CTL) {
        mix_block(o, CTL);
        if (rec)
            for (i = 0; i < CTL && f + i < n44; i++) {
                int32_t m = (o[2u * i] + o[2u * i + 1u]) >> 1;
                if ((f + i) & 1u) {                /* every other sample: 1 2 1 over the pair */
                    int32_t y = (be.prev + 2 * be.odd + m) >> 2;
                    bnc_put(k, clamp(y, -32768, 32767));
                    be.prev = m;
                } else
                    be.odd = m;
            }
        if (!(f & 2047u))
            bnc_progress(rec ? 2u : 1u, (uint32_t)(((uint64_t)(rec ? n44 + f : f) * 100u) / total));
    }
}

/* SAVE > BOUNCE's OCT+: 0 done, else what failed (said) */
static int bnc_go(void)
{
    uint32_t k = bnc.to % SMP_USER_SLOTS, n22 = bnc_len(), n44 = 2u * n22, i, sel = song.sel;
    uint8_t mute[NTRK], lowcut = fx_lowcut;
    uint16_t master = song.master_q12;
    const char *why = bnc_why();
    smp_user_hdr_t *h;
    if (why) {
        ui_message(why);
        return 1;
    }
    if (song.playing || seq_counting())
        seq_stop();
    transport_req = 0;
    for (i = 0; i < NTRK; i++) {                    /* FROM T1..T4: that track alone */
        mute[i] = (uint8_t)trk[i].p[P_MUTE];
        trk[i].p[P_MUTE] = (int16_t)!bnc_renders(i);
        trk_all_off(&trk[i]);
    }
    bnc_on = 1;                                    /* the audio interrupt: quiet, no mix of its own (audio.c) */
    song.master_q12 = MASTER_FULL;
    fx_lowcut = 0;
    memset(&be, 0, sizeof be);
    be.crc = 0xFFFFFFFFu;
    be.at = SMP_USER_DATA;
    /* the slot is invalid from here (its voices end): the header sector erased, as the editor's SMP_BEGIN */
    usr_nz[k] = 0;
    for (i = 0; i < 16u; i++)
        usr_zone[k][i].n = 0;
    if (bnc_erase(k, 0))
        be.err = 2;
    smp_user_scan(k);                              /* (no header: empty; SHARD's table of it too) */
    bnc.last_ms = fm1_ms - 100u;
    seq_start();                                   /* the run-in: unheard */
    bnc_render(k, n44, 0);
    seq_stop();
    seq_start();                                   /* from the top again (the tails ring on): recorded */
    bnc_render(k, n44, 1);
    seq_stop();
    if (be.n & 511u)
        bnc_flush(k, ((be.n & 511u) + 1u) >> 1);
    bnc_progress(3u, 100u);
    if (!be.err) {                                 /* the header last: the slot is valid from here */
        static uint8_t hb[sizeof(smp_user_hdr_t)] __attribute__((aligned(4), section(".pool")));
        static const char NM[] = "BOUNCE";
        memset(hb, 0, sizeof hb);
        h = (smp_user_hdr_t *)hb;
        h->magic = SMP_USER_MAGIC;
        h->version = 1;
        h->nz = 1;
        memcpy(h->name, NM, sizeof NM - 1u);
        h->name[6] = (char)('0' + BNC_BARS[bnc.bars & 3u] % 10u);   /* "BOUNCE4" */
        h->data_len = be.at - SMP_USER_DATA;
        h->crc = ~be.crc;
        h->zone[0].n = be.n;
        h->zone[0].le = be.n - 1u;
        h->zone[0].rate = 32768u;                  /* 22,050 Hz (Q16 of 44,100) */
        h->zone[0].root16 = 60 * 16;
        h->zone[0].hi = 127;
        if (bnc_prog(k, 0, hb, sizeof hb))
            be.err = 3;
    }
    song.master_q12 = master;
    fx_lowcut = lowcut;
    for (i = 0; i < NTRK; i++)
        trk[i].p[P_MUTE] = mute[i];
    song.sel = (uint8_t)sel;
    smp_user_scan(k);                              /* the new material: SHARD's slices and tempo */
    bnc_on = 0;
    bnc.busy = 0;
    if (be.err || !usr_nz[k]) {
        ui_message(be.err ? "BOUNCE: FLASH ERROR" : "BOUNCE FAILED");
        return 1;
    }
    {
        uint32_t src = slc_src_of_slot(k), bpm = (uint32_t)song.g[G_BPM];
        if (n22 == bnc_want() && bpm >= SHD_BPM_MIN && bpm <= SHD_BPM_MAX)
            shd_kit[src].bus.bpm = (uint8_t)bpm;    /* exactly BARS bars at the song's tempo */
        shd_kit_save |= (uint8_t)(1u << k);          /* (with the sample: slice_store.c, once stopped) */
    }
    ui_say("BOUNCED TO ", SMP_ALL_NAMES[SMP_NSETS + k]);
    return 0;
}

/* ---- the page: BARS FROM TO, OCT+ BOUNCE (ui_draw.c, ui_input.c, ui.c act_*) */
static void bnc_knob(uint32_t slot, int32_t steps)
{
    if (bnc.busy)
        return;
    if (slot == 0u)
        bnc.bars = (uint8_t)clamp((int32_t)bnc.bars + steps, 0, 3);
    else if (slot == 1u)
        bnc.from = (uint8_t)clamp((int32_t)bnc.from + steps, 0, NTRK);
    else if (slot == 2u)
        bnc.to = (uint8_t)clamp((int32_t)bnc.to + steps, 0, SMP_USER_SLOTS - 1);
}
/* card c: label, value, unit, gauge (0..1000, -1 none); 0 = the action's own column (draw_act_column) */
static int bnc_card(uint32_t c, const char **lb, char *val, const char **u, int32_t *r)
{
    static const char *const FROM[1 + NTRK] = {"MIX", "T1", "T2", "T3", "T4"};
    *u = "";
    *r = -1;
    if (c == 0u) {
        *lb = "BARS";
        fmt_int(val, BNC_BARS[bnc.bars & 3u]);
        *r = (int32_t)bnc.bars * 1000 / 3;
        return 1;
    }
    if (c == 1u) {
        *lb = "FROM";
        str_cpy(val, FROM[bnc.from % (1u + NTRK)], 8);
        return 1;
    }
    if (c == 2u) {
        *lb = "TO";
        str_cpy(val, SMP_ALL_NAMES[SMP_NSETS + bnc.to % SMP_USER_SLOTS], 8);   /* (used or not: the panel says) */
        return 1;
    }
    return 0;
}
static int bnc_ready(void) { return !bnc.busy && !bnc_why(); }
static uint32_t bnc_sig(void)
{
    return (uint32_t)bnc.bars | (uint32_t)bnc.from << 2 | (uint32_t)bnc.to << 5 | (uint32_t)bnc.busy << 8 |
           (uint32_t)bnc.pct << 10 | (uint32_t)song.g[G_BPM] << 17 | (uint32_t)usr_nz[bnc.to % SMP_USER_SLOTS] << 26 |
           (uint32_t)(bnc_why() != 0) << 27;
}
#endif

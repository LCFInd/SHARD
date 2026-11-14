/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SLICES (EDIT family, a SLICE track only; after EDIT 2): the slices of the selected part's sample, set by hand.
 * Based on hugelton/Felucca#27 by andreahaku (SLICE EDIT), on the current UI: a page of the EDIT family with the
 * action-page OCT+ / OCT- of SAVE > TOOLS.
 * The markers: the slice starts the track plays (its DIV: 4..32 equal, SHARD's 2..32, AUTO, MAN) and the END of the last slice.
 *   KNOB 1  SLICE  the marker (1..n, then END); a key pressed picks the slice it plays (its keys are lit)
 *   KNOB 2  POS    moves it: one column of the view per detent; the view spans its neighbours, so a marker between
 *                  close ones moves in fine steps. Slice 1's start trims the head, END the tail
 *   KNOB 3  SPLIT  picked (right): OCT+ splits the slice at its middle (up to 32 slices)
 *   KNOB 4  JOIN   picked (right): OCT+ joins the slice to the one before (its start goes)
 *   OCT-    drops the pick, or (none) goes HOME
 * Edits are for a user slot (USR1..3: SRC); BREAK and PIANO show their slices only, a source with no material (it
 * plays a sine) says SAMPLE NOT FOUND (the no-file icon). The first edit takes
 * the slices shown into the slot's MAN table (eng_slice.c) and sets DIV to MAN; later edits change that table (the
 * audio side switches tables between notes). Leaving the page saves them with the sample (slice_store.c, once
 * stopped). Included by ui.c (before the action pages); the cards: ui_draw.c, the waveform: ui_graph.c. */
#if FELUCCA_SLICE
#define SP_COLS 216u                                 /* the waveform's columns (the panel's x 12..227) */
static struct {
    uint8_t sel;                                     /* the marker: slice 0..n - 1, n = END */
    uint8_t env_src;                                 /* what lo / hi were decoded for */
    uint32_t env_a, env_b, env_len, env_gen;         /* (env_gen: smp_user_gen, a re-upload of the same length) */
    int8_t lo[SP_COLS], hi[SP_COLS];                 /* per column: smallest / largest sample >> 9 */
} sp;

static int slice_page_ok(void)
{
    const engine_t *e = ENGINES[TSEL->eng_req % NENGINES];
#if FELUCCA_SHARD
    if (e == &ENG_SHARD)                             /* SHARD plays SLICE's material and slices (E0..E2 alike) */
        return 1;
#endif
    return ENG_REF_IS(e, ENG_SLICE);
}
static int slice_page_on(void)
{
    return !ui.home && (cur_page()->graph == GR_SLICES || cur_page()->graph == GR_SHFX || cur_page()->graph == GR_LOOP) &&
           slice_page_ok();
}
/* the selected track's DIV as SLICE's code (SHARD lists its own: 2 .. 32, AUTO, MAN), and its MAN value */
static uint32_t slice_div_code(void)
{
#if FELUCCA_SHARD
    if (ENGINES[TSEL->eng_req % NENGINES] == &ENG_SHARD)
        return shd_div(TSEL->p);
#endif
    return (uint32_t)clamp(TSEL->p[P_E1], 0, SLC_DIV_MAN);
}
static int16_t slice_div_man(void)
{
#if FELUCCA_SHARD
    if (ENGINES[TSEL->eng_req % NENGINES] == &ENG_SHARD)
        return SHD_DIV_MAN;
#endif
    return SLC_DIV_MAN;
}

/* the source the selected part plays, its DIV; 0 = no material (an empty slot: the sine, as slice_note_on) */
static const slc_src_t *slice_src(uint32_t *src, uint32_t *div)
{
    const int16_t *p = TSEL->p;
    uint32_t k = slc_src_of(p);
    const slc_src_t *s = slc_get(k);
    *src = k;
    *div = slice_div_code();
    return s;
}

/* the markers shown: n slices, marker i's position (i = n: the end) */
static uint32_t slice_count(void)
{
    uint32_t src, div;
    const slc_src_t *s = slice_src(&src, &div);
    return s ? slc_count(s, div) : 0u;
}
static uint32_t slice_mark(uint32_t i)
{
    uint32_t src, div, n, a, b, st;
    const slc_src_t *s = slice_src(&src, &div);
    if (!s || !(n = slc_count(s, div)))
        return 0;
    slc_bounds(s, div, i < n ? i : n - 1u, &a, &b, &st);
    return i < n ? a : b;
}
static uint32_t slice_sel(void)
{
    uint32_t n = slice_count();
    return sp.sel < n ? sp.sel : n;
}

/* the view: from the marker before the selected one to the marker after it (the head / the material's end), so the
 * marker's whole range of motion is on the screen; *len the material's samples */
static void slice_view(uint32_t *a, uint32_t *b, uint32_t *len)
{
    uint32_t src, div, n = slice_count(), j = slice_sel();
    const slc_src_t *s = slice_src(&src, &div);
    *len = s ? s->len : 0u;
    *a = j ? slice_mark(j - 1u) : 0u;
    *b = j < n ? slice_mark(j + 1u) : *len;
    if (j + 1u == n && *b < *len)                    /* the last slice: up to the material's end (its END moves) */
        *b = *len;
    if (*b <= *a)
        *b = *a + 1u;
}

/* the edit table of the selected part's slot, the first time from the slices shown (DIV becomes MAN); 0 = BREAK,
 * PIANO or an empty slot: nothing to edit (says so) */
static slc_man_t *slice_edit_begin(uint32_t *k)
{
    uint32_t src, div, n, i, a, b, st;
    const slc_src_t *s = slice_src(&src, &div);
    slc_man_t *m;
    if (!s || slc_slot_of_src(src) >= SMP_USER_SLOTS) {
        static const char *const EMPTY[4] = {"USR1 EMPTY", "USR2 EMPTY", "USR3 EMPTY", "USR4 EMPTY"};
        ui_message(slc_slot_of_src(src) < SMP_USER_SLOTS ? EMPTY[slc_slot_of_src(src)] :
                   SMP_USER_SLOTS > 3 ? "SRC USR1-4 TO EDIT" : "SRC USR1-3 TO EDIT");
        return 0;
    }
    *k = slc_slot_of_src(src);
    if (div != SLC_DIV_MAN && slc_man_of(s) && ENGINES[TSEL->eng_req % NENGINES] != &ENG_SHARD) {
        TSEL->p[P_E1] = slice_div_man();             /* SLICE: the slot has slices set by hand: those, not the grid; */
        ui_message("DIV MAN");                       /* this touch only shows them (its marker was another one) */
        return 0;
    }                                                /* SHARD: what is shown is what is edited, the old MAN replaced */
    m = slc_man_begin(*k);
    if (m && div != SLC_DIV_MAN) {                   /* the slices shown (2..32 equal, AUTO) become the MAN table */
        n = slc_count(s, div);
        m->n = 0;
        m->end = 0;
        for (i = 0; i < n; i++) {
            slc_bounds(s, div, i, &a, &b, &st);
            if (a < s->len && s->len - a >= SLC_MIN && (!m->n || a - m->pos[m->n - 1u] >= SLC_MIN)) {
                m->pos[m->n] = a;
                m->st[m->n++] = st;
            }
        }
    }
    if (!m || !m->n) {
        ui_message("SAMPLE TOO SHORT");
        return 0;
    }
    return m;
}
static void slice_edit_end(uint32_t k)
{
    slc_man_commit(k);
    if (TSEL->p[P_E1] != slice_div_man()) {
        TSEL->p[P_E1] = slice_div_man();                /* (the editor sees it as any change of the value) */
        ui_message("DIV MAN");
    }
    slc_man_save |= (uint8_t)(1u << k);              /* to flash when the page is left (slice_store.c) */
}

/* KNOB 1: the marker; KNOB 2: move it */
static void slice_knob(uint32_t slot, int32_t steps)
{
    uint32_t n = slice_count(), j = slice_sel(), k, a, b, len;
    slc_man_t *m;
    if (!n) {                                        /* no material (a sine plays): KNOB 2 says why ("USR2 EMPTY") */
        if (slot)
            slice_edit_begin(&k);
        return;
    }
    if (slot == 0u) {
#if FELUCCA_SHARD
        if (j >= n && steps > 0 && ENGINES[TSEL->eng_req % NENGINES] == &ENG_SHARD)
            ui_message("BUS IS ON SLICE FX");          /* (past END: the BUS is the next page's, the same marker) */
#endif
        sp.sel = (uint8_t)clamp((int32_t)j + steps, 0, (int32_t)n);
        return;
    }
    slice_view(&a, &b, &len);
    if (!(m = slice_edit_begin(&k)))
        return;
    j = j < n ? (j < m->n ? j : m->n - 1u) : m->n;   /* (a table cut short by SLC_MIN: the nearest) */
    {
        int32_t d = steps * (int32_t)((b - a) / SP_COLS ? (b - a) / SP_COLS : 1u);
        const slc_src_t *s = slc_get(slc_src_of_slot(k));
        if (j < m->n)
            slc_man_move(s, m, j, d);
        else
            slc_man_move_end(s, m, d);
    }
    slice_edit_end(k);
    sp.sel = (uint8_t)j;
}

/* OCT+ would do it: SPLIT (c 2) a slice of a user slot below 32 slices and long enough, JOIN (c 3) a slice but the
 * first; on the END marker neither */
static int slice_act_ready(uint32_t c)
{
    uint32_t src, div, n = slice_count(), j = slice_sel();
    const slc_src_t *s = slice_src(&src, &div);
    if (!s || slc_slot_of(s) >= SMP_USER_SLOTS || j >= n)   /* (BREAK, PIANO: no edits) */
        return 0;
    if (c == 3u)
        return j > 0u;
    return n < SLC_AUTO && slice_mark(j + 1u) - slice_mark(j) >= 2u * SLC_MIN;
}
static void slice_act(uint32_t c)
{
    uint32_t n = slice_count(), j = slice_sel(), k;
    slc_man_t *m;
    if (!n) {                                        /* no material: says why */
        slice_edit_begin(&k);
        return;
    }
    if (j >= n) {
        ui_message("PICK A SLICE");
        return;
    }
    if (!(m = slice_edit_begin(&k)))
        return;
    j = j < m->n ? j : m->n - 1u;
    if (c == 3u) {
        if (!j) {
            ui_message("FIRST SLICE");
            return;
        }
        j = slc_man_join(m, j);
    } else {
        uint32_t before = m->n;
        j = slc_man_split(slc_get(slc_src_of_slot(k)), m, j);
        if (m->n == before) {
            ui_message(before >= SLC_AUTO ? "32 SLICES AT MOST" : "SLICE TOO SHORT");
            return;
        }
    }
    slice_edit_end(k);
    sp.sel = (uint8_t)j;
}

/* a key pressed on the page: its slice becomes the marker (the key plays it as always) */
static void slice_keys_pick(uint32_t pressed)
{
    uint32_t n = slice_count(), k;
    for (k = 0; n && k < 27u; k++)
        if ((pressed >> k) & 1u) {
            uint32_t note = kb_map(TSEL, k);
            if (note != KB_SILENT)
                sp.sel = (uint8_t)slc_note_slice(TSEL->p, note, n);
        }
}

/* ui_leds: the keys that play the selected slice (bit k = key k), as kb_map and slice_note_on map them */
static uint32_t slice_leds(void)
{
    uint32_t n = slice_count(), j = slice_sel(), k, mask = 0;
    for (k = 0; j < n && k < 27u; k++) {
        uint32_t note = kb_map(TSEL, k);
        if (note != KB_SILENT && slc_note_slice(TSEL->p, note, n) == j)
            mask |= 1u << k;
    }
    return mask;
}

/* the waveform of [a, b): per column the smallest and largest sample (main loop, when the view moves) */
static void slice_env(uint32_t a, uint32_t b)
{
    uint32_t src, div, c, e, len;
    const slc_src_t *s = slice_src(&src, &div);
    slc_dec_t d;
    len = s ? s->len : 0u;
    if (!s || (sp.env_a == a && sp.env_b == b && sp.env_len == len && sp.env_src == src &&
               sp.env_gen == smp_user_gen))
        return;
    slc_dec_at(&d, a, slc_state_at(s, a));
    for (c = 0; c < SP_COLS; c++) {
        int32_t lo = 32767, hi = -32768;
        e = a + (uint32_t)((uint32_t)(c + 1u) * (b - a) / SP_COLS);
        do {                                         /* at least one sample per column */
            int32_t x = d.pos < len ? slc_dec_next(s, &d) : 0;
            lo = x < lo ? x : lo;
            hi = x > hi ? x : hi;
        } while (d.pos < e);
        sp.lo[c] = (int8_t)(lo >> 9);
        sp.hi[c] = (int8_t)(hi >> 9);
    }
    sp.env_a = a;
    sp.env_b = b;
    sp.env_len = len;
    sp.env_gen = smp_user_gen;
    sp.env_src = (uint8_t)src;
}

/* "1.234": samples of s as seconds */
static void slice_time(char *b, uint32_t n)
{
    uint32_t src, div, hz;
    const slc_src_t *s = slice_src(&src, &div);
    hz = s ? (s->rate >> 4) * 44100u >> 12 : 0u;    /* (as slc_scan) */
    fmt_fix(b, (int32_t)((uint32_t)n * 1000u / (hz ? hz : 1u)), 3);
    if (b[0] == '.') {                               /* fmt_fix writes ".255": "0.255" */
        uint32_t i = str_len(b) + 1u;
        for (; i; i--)
            b[i] = b[i - 1u];
        b[0] = '0';
    }
}

/* graph_signature: what the page shows */
static uint32_t slice_sig(void)
{
    uint32_t n = slice_count(), i, h = n * 131u + slice_sel() * 7919u + smp_user_gen * 104729u;
    for (i = 0; i <= n; i++)
        h = (h ^ slice_mark(i)) * 16777619u;
    return h;
}

#if FELUCCA_SHARD
/* SLICE FX (EDIT family, a SHARD track only; after SLICES): each slice's settings and the bus (eng_shard.c's kit).
 *   KNOB 1  SLICE  the slice (1..n), then BUS (the multiband distortion); a key pressed picks its slice
 *   KNOB 2  PARAM  what to set: PITCH LEVEL ATTACK DECAY MODE REV FILTER CUT RES DRIVE AMT ROUTE CHOKE; on BUS
 *                  the source's BPM (AUTO: the tempo found, shown), the split points (XLOW, XHIGH) and each band's
 *                  (L M H) TYPE DRV GATE LVL
 *   KNOB 3  VALUE  the slice's (or the bus's) value
 *   KNOB 4  ALL    every slice's value moves by the same steps (BUS: none)
 * The kit is the source's: a user slot's is kept with its sample when the page is left (slice_store.c, once
 * stopped); BREAK's and PIANO's last until the power goes. Slices past 32 (none: SLC_AUTO) share the last's. */
typedef struct {
    const char *name;
    int16_t min, max;
    const char *const *names;
    const char *unit;
} shfx_desc_t;
enum { SHFX_PITCH, SHFX_LEVEL, SHFX_ATK, SHFX_DEC, SHFX_SUS, SHFX_REL, SHFX_MODE, SHFX_DIR, SHFX_GRAIN, SHFX_GSIZE,
       SHFX_GDENS, SHFX_GJIT, SHFX_FILT, SHFX_CUT, SHFX_RES, SHFX_DRIVE, SHFX_AMT, SHFX_DLY, SHFX_ROUTE, SHFX_CHOKE,
       SHFX_NSL };
static const shfx_desc_t SHFX_SL[SHFX_NSL] = {
    {"PITCH", -24, 24, 0, "ST"}, {"LEVEL", 0, 127, 0, ""}, {"ATTACK", 0, 127, 0, ""}, {"DECAY", 0, 127, 0, ""},
    {"SUS", 0, 127, 0, ""}, {"REL", 0, 127, 0, ""}, {"MODE", 0, 3, N_SHD_SMODE, ""}, {"DIR", 0, SHD_NDIR - 1, N_SHD_DIR, ""},
    {"GRAIN", 0, SHD_NGM, N_SHD_SGRAIN, ""}, {"SIZE", 0, 127, 0, "MS"}, {"DENS", 0, 127, 0, "X"}, {"JITTER", 0, 127, 0, ""},
    {"FILTER", 0, SHD_NFILT - 1, N_SHD_FILT, ""}, {"CUT", 0, 127, 0, ""}, {"RES", 0, 127, 0, ""},
    {"DRIVE", 0, SHD_NDRIVE - 1, N_SHD_DRIVE, ""}, {"AMT", 0, 127, 0, ""}, {"DELAY", 0, 127, 0, ""},
    {"ROUTE", 0, SHD_NROUTE - 1, N_SHD_ROUTE, ""}, {"CHOKE", 0, 4, N_SHD_CHOKE, ""},
};
enum { SHFX_BPM, SHFX_SENS, SHFX_BGRAIN, SHFX_BGSIZE, SHFX_BGDENS, SHFX_BGJIT, SHFX_XLOW, SHFX_XHIGH, SHFX_CH0 };   /* then TYPE DRV FDBK GATE LVL per channel */
#define SHFX_PER 5u
static const shfx_desc_t SHFX_BUS[SHFX_CH0 + SHFX_PER * SHD_NCH + 1u] = {   /* (6 letters at most: the cards) */
    {"LOOP", 0, SHD_BPM_MAX, 0, ""},                 /* the source's tempo (BPM), 0 AUTO (shfx_knob); the song's: TEMPO */
    {"SENS", 0, 127, 0, ""},                         /* AUTO's hits (a user slot's: slc_rescan) */
    {"GRAIN", 0, SHD_NGM - 1, N_SHD_GRAIN, ""},       /* the track's GRAIN: the slices set to TRACK */
    {"SIZE", 0, 127, 0, "MS"}, {"DENS", 0, 127, 0, "X"}, {"JITTER", 0, 127, 0, ""},
    {"XLOW", 0, 127, 0, ""}, {"XHIGH", 0, 127, 0, ""},
    {"L TYPE", 0, SHD_NDRIVE - 1, N_SHD_DRIVE, ""}, {"L DRV", 0, 127, 0, ""}, {"L FDBK", 0, 127, 0, ""},
    {"L GATE", 0, 127, 0, ""}, {"L LVL", 0, 127, 0, ""},
    {"M TYPE", 0, SHD_NDRIVE - 1, N_SHD_DRIVE, ""}, {"M DRV", 0, 127, 0, ""}, {"M FDBK", 0, 127, 0, ""},
    {"M GATE", 0, 127, 0, ""}, {"M LVL", 0, 127, 0, ""},
    {"H TYPE", 0, SHD_NDRIVE - 1, N_SHD_DRIVE, ""}, {"H DRV", 0, 127, 0, ""}, {"H FDBK", 0, 127, 0, ""},
    {"H GATE", 0, 127, 0, ""}, {"H LVL", 0, 127, 0, ""},
    {"F TYPE", 0, SHD_NDRIVE - 1, N_SHD_DRIVE, ""}, {"F DRV", 0, 127, 0, ""}, {"F FDBK", 0, 127, 0, ""},
    {"F GATE", 0, 127, 0, ""}, {"F LVL", 0, 127, 0, ""},
    {"TEMPO", 40, 240, 0, "BPM"},                    /* the song's tempo (song.g[G_BPM]), last (shfx_knob) */
};
#define SHFX_TEMPO (SHFX_CH0 + SHFX_PER * SHD_NCH)
/* SENS: a user slot's AUTO slices again once the knob rests (slc_rescan stops its voices: not at every step) */
static uint8_t shd_sens_pend;                       /* bit k: user slot k */
static uint32_t shd_sens_at;
static void shd_sens_poll(void)                     /* ui_input, every frame */
{
    uint32_t k;
    if (!shd_sens_pend || fm1_ms - shd_sens_at < 300u)
        return;
    for (k = 0; k < SMP_USER_SLOTS; k++)
        if (shd_sens_pend >> k & 1u)
            slc_rescan(k, shd_kit[slc_src_of_slot(k)].bus.sens);
    shd_sens_pend = 0;
    ui.force = 1;
}
static uint8_t shfx_sl_param, shfx_bus_param;

/* the kit of the selected part's source; *bus 1 on the BUS marker, *j the slice */
static shd_kit_t *shfx_kit(uint32_t *src, uint32_t *j, int *bus)
{
    uint32_t div, n = slice_count();
    slice_src(src, &div);
    *j = slice_sel();
    *bus = *j >= n;
    return *src < SHD_NSRC ? &shd_kit[*src] : 0;
}
/* the byte of slice j's setting k, or the bus's k */
static uint8_t *shfx_field(shd_kit_t *kit, uint32_t j, int bus, uint32_t k)
{
    shd_sl_t *s;
    if (bus) {
        shd_bus_t *b = &kit->bus;
        uint32_t c = (k - SHFX_CH0) / SHFX_PER % SHD_NCH;
        switch (k) {
        case SHFX_BPM: return &b->bpm;
        case SHFX_SENS: return &b->sens;
        case SHFX_XLOW: return &b->lo;
        case SHFX_XHIGH: return &b->hi;
        case SHFX_BGRAIN: return &b->gmode;
        case SHFX_BGSIZE: return &b->gsize;
        case SHFX_BGDENS: return &b->gdens;
        case SHFX_BGJIT: return &b->gjit;
        default: break;
        }
        switch ((k - SHFX_CH0) % SHFX_PER) {
        case 0: return &b->type[c];
        case 1: return &b->drive[c];
        case 2: return &b->fb[c];
        case 3: return &b->gate[c];
        default: return &b->level[c];
        }
    }
    s = &kit->sl[j % SLC_AUTO];
    switch (k) {
    case SHFX_PITCH: return (uint8_t *)&s->pitch;
    case SHFX_LEVEL: return &s->level;
    case SHFX_ATK: return &s->atk;
    case SHFX_DEC: return &s->dec;
    case SHFX_SUS: return &s->sus;
    case SHFX_REL: return &s->rel;
    case SHFX_MODE: return &s->mode;
    case SHFX_DIR: return &s->rev;
    case SHFX_FILT: return &s->filt;
    case SHFX_CUT: return &s->cut;
    case SHFX_RES: return &s->res;
    case SHFX_DRIVE: return &s->dtype;
    case SHFX_AMT: return &s->drv;
    case SHFX_DLY: return &s->dly;
    case SHFX_ROUTE: return &s->route;
    case SHFX_GRAIN: return &s->grain;
    case SHFX_GSIZE: return &s->gsize;
    case SHFX_GDENS: return &s->gdens;
    case SHFX_GJIT: return &s->gjit;
    default: return &s->choke;
    }
}
static int32_t shfx_get(const uint8_t *f, uint32_t k, int bus) { return !bus && k == SHFX_PITCH ? (int8_t)*f : *f; }
static void shfx_set(uint8_t *f, const shfx_desc_t *d, int32_t v) { *f = (uint8_t)(int8_t)clamp(v, d->min, d->max); }
static void shfx_changed(uint32_t src)
{
    if (slc_slot_of_src(src) < SMP_USER_SLOTS)
        shd_kit_save |= (uint8_t)(1u << slc_slot_of_src(src));   /* to flash with the sample when the page is left */
}

static void shfx_knob(uint32_t slot, int32_t steps)
{
    uint32_t src, j, n = slice_count(), i;
    int bus;
    shd_kit_t *kit = shfx_kit(&src, &j, &bus);
    const shfx_desc_t *d;
    uint32_t k;
    if (!n || !kit) {                                /* no material: says why (as SLICES) */
        if (slot)
            ui_message("SAMPLE NOT FOUND");
        return;
    }
    if (slot == 0u) {
        sp.sel = (uint8_t)clamp((int32_t)j + steps, 0, (int32_t)n);
        return;
    }
    k = bus ? shfx_bus_param : shfx_sl_param;
    if (slot == 1u) {
        if (bus)
            shfx_bus_param = (uint8_t)clamp((int32_t)k + steps, 0, NELEM(SHFX_BUS) - 1);
        else
            shfx_sl_param = (uint8_t)clamp((int32_t)k + steps, 0, NELEM(SHFX_SL) - 1);
        return;
    }
    d = bus ? &SHFX_BUS[k] : &SHFX_SL[k];
    if (slot == 2u && bus && k == SHFX_SENS) {       /* SENS: a user slot's; BREAK's and PIANO's slices are fixed */
        if (slc_slot_of_src(src) >= SMP_USER_SLOTS) {
            ui_message("SENS: USER SAMPLES");
            return;
        }
        shfx_set(&kit->bus.sens, d, kit->bus.sens + steps);
        shd_sens_pend |= (uint8_t)(1u << slc_slot_of_src(src));
        shd_sens_at = fm1_ms;
    } else if (slot == 2u && bus && k == SHFX_TEMPO) {   /* TEMPO: the song's, as SELECT turned it */
        song.g[G_BPM] = (int16_t)clamp(song.g[G_BPM] + steps, GP[G_BPM].min, GP[G_BPM].max);
        ui.bpm_t = 40;
        return;                                      /* (not the kit's: nothing to save with the sample) */
    } else if (slot == 2u && bus && k == SHFX_BPM) { /* BPM: up from AUTO starts at the tempo found (else 120); */
        uint8_t *f = &kit->bus.bpm;                  /* below 40 is AUTO again */
        int32_t v = *f;
        if (!v)
            v = steps > 0 ? (int32_t)(shd_det_bpm10[src] ? (shd_det_bpm10[src] + 5u) / 10u : 120u) - 1 + steps : 0;
        else
            v += steps;
        *f = (uint8_t)(v < (int32_t)SHD_BPM_MIN ? 0 : v > (int32_t)SHD_BPM_MAX ? (int32_t)SHD_BPM_MAX : v);
    } else if (slot == 2u) {
        uint8_t *f = shfx_field(kit, j, bus, k);
        shfx_set(f, d, shfx_get(f, k, bus) + steps);
    } else if (!bus) {                               /* ALL: every slice by the same steps */
        for (i = 0; i < n && i < SLC_AUTO; i++) {
            uint8_t *f = shfx_field(kit, i, 0, k);
            shfx_set(f, d, shfx_get(f, k, 0) + steps);
        }
    } else
        return;
    shfx_changed(src);
}

/* card c: its label, value text, unit and gauge (0..1000, -1 none); 0 = an empty card */
static int shfx_card(uint32_t c, const char **lb, char *val, const char **u, int32_t *r)
{
    uint32_t src, j, n = slice_count(), k;
    int bus, v;
    shd_kit_t *kit = shfx_kit(&src, &j, &bus);
    const shfx_desc_t *d;
    *u = "";
    *r = -1;
    if (!n || !kit)
        return 0;
    k = bus ? shfx_bus_param : shfx_sl_param;
    d = bus ? &SHFX_BUS[k] : &SHFX_SL[k];
    if (c == 0u) {
        *lb = "SLICE";
        if (bus)
            str_cpy(val, "BUS", 8);
        else
            fmt_int(val, (int32_t)j + 1);
        *u = bus ? "" : "/";
        if (!bus) {
            static char nn[8];
            nn[0] = '/';
            fmt_int(nn + 1, (int32_t)n);
            *u = nn;
        }
        return 1;
    }
    if (c == 1u) {
        *lb = "PARAM";
        str_cpy(val, d->name, 12);
        return 1;
    }
    if (c == 3u && bus && k == SHFX_BPM) {          /* BPM: the tempo found beside it (0.1 BPM) */
        *lb = "FOUND";
        if (shd_det_bpm10[src])
            fmt_fix(val, (int32_t)shd_det_bpm10[src], 1);
        else
            str_cpy(val, "--", 12);
        return 1;
    }
    if (c == 3u && bus)
        return 0;
    if (bus && k == SHFX_TEMPO) {                    /* the song's tempo */
        *lb = d->name;
        fmt_int(val, song.g[G_BPM]);
        *u = d->unit;
        *r = (song.g[G_BPM] - 40) * 1000 / 200;
        return 1;
    }
    v = shfx_get(shfx_field(kit, j, bus, k), k, bus);
    *lb = c == 2u ? d->name : "ALL";
    if (bus && k == SHFX_SENS && slc_slot_of_src(src) >= SMP_USER_SLOTS) {
        str_cpy(val, "FIXED", 12);                   /* (BREAK, PIANO: slices made with the firmware) */
        return 1;
    }
    if (bus && k == SHFX_BPM) {                      /* BPM: set, or AUTO (the tempo found: the next card) */
        if (v) {
            fmt_int(val, v);
            *u = "BPM";
            *r = (v - (int32_t)SHD_BPM_MIN) * 1000 / (int32_t)(SHD_BPM_MAX - SHD_BPM_MIN);
        } else
            str_cpy(val, "AUTO", 12);
        return 1;
    }
    if (d->names)
        str_cpy(val, d->names[clamp(v, d->min, d->max) - d->min], 12);
    else if (!bus && k == SHFX_DEC && v >= 127)
        str_cpy(val, "NONE", 12);                    /* DECAY 127: no decay */
    else if (!bus && (k == SHFX_REL || k == SHFX_DLY) && !v)
        str_cpy(val, "OFF", 12);                     /* RELEASE 0: the track's; DELAY 0: no send */
    else if ((!bus && k == SHFX_GSIZE) || (bus && k == SHFX_BGSIZE)) {
        fmt_int(val, (int32_t)((shd_glen((uint32_t)v) * 10u + 220u) / 441u));   /* SIZE: ms */
        *u = d->unit;
    } else if ((!bus && k == SHFX_GDENS) || (bus && k == SHFX_BGDENS)) {
        fmt_fix(val, (int32_t)(shd_gov((uint32_t)v) * 10u / 256u), 1);           /* DENS: grains overlapping */
        *u = d->unit;
    }
    else {
        if (d->min < 0 && v > 0) {
            val[0] = '+';
            fmt_int(val + 1, v);
        } else
            fmt_int(val, v);
        *u = d->unit;
    }
    *r = d->max > d->min ? (v - d->min) * 1000 / (d->max - d->min) : -1;
    return 1;
}

/* graph_signature: the kit's bytes the cards show */
static uint32_t shfx_sig(void)
{
    uint32_t src, j, n = slice_count(), h = shfx_sl_param * 31u + shfx_bus_param * 131u, i;
    int bus;
    shd_kit_t *kit = shfx_kit(&src, &j, &bus);
    if (!kit)
        return h;
    for (i = 0; i < n && i < SLC_AUTO; i++)
        h = (h ^ *shfx_field(kit, i, 0, shfx_sl_param)) * 16777619u;
    for (i = 0; i < NELEM(SHFX_BUS); i++)
        h = (h ^ *shfx_field(kit, 0, 1, i)) * 16777619u;
    h = (h ^ shd_det_bpm10[src % SHD_NSRC]) * 16777619u;           /* (BPM AUTO: the tempo found) */
    h = (h ^ (uint32_t)song.g[G_BPM]) * 16777619u;                   /* (TEMPO) */
    return h;
}
#endif
#else
static int slice_page_ok(void) { return 0; }
static int slice_page_on(void) { return 0; }
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* FELUCCA + SHARD browser emulator: the firmware's DSP, sequencer and UI sources (the same unity build as
 * tests/ui_test.c) against a stub HAL, compiled to wasm32 (freestanding, no libc).
 * JS drives it: emu_init, emu_frame (16 ms of UI), emu_render (CTL-sample audio blocks), the button / key / knob
 * setters, emu_slot_loaded after writing a user sample slot into emu_slot_ptr(k). */
#include <stddef.h>
#include <stdint.h>
#define FELUCCA_CDC 1
#define FELUCCA_EMU 1                    /* (firmware: what only the device can do) */
#define __attribute__(x)
#define EXPORT(name)
static uint32_t host_slots[4u * 0x14000u / 4u];          /* USR1..3, as the flash at 0xA0000 */
#define SMP_USER_XIP(k) ((const uint8_t *)host_slots + (k) * SMP_USER_SIZE)
#include "felucca_tables.h"
#include "../../firmware/bsp/chip/libc.c"
static struct { volatile uint32_t notes, buttons; } fm1_in;
#include "../../firmware/src/core.h"
#include "../../firmware/src/engines.c"
#include "../../firmware/src/params.c"
#include "../../firmware/src/mod.c"
#include "../../firmware/src/voice.c"
#include "../../firmware/src/slicer.c"
#include "../../firmware/src/fx.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../../firmware/bsp/chip/usb.c"
#include "../../firmware/src/midi_uart.c"
#include "../../firmware/src/song_chain.c"
#include "../../firmware/src/seq.c"

/* ------------------------------------------------------------ HAL stubs --- */
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {                 /* bsp/boards/fm1/fm1_input.h */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL], fm1_led_mid[FM1_NCOL];
static void fm1_led_dim_level(uint32_t lo) { (void)lo; }
static int fm1_led_anim_on(void) { return 0; }
static void fm1_led_idle_start(uint32_t glow) { (void)glow; }
static int fm1_led_idle_on(void) { return 0; }
static void fm1_led_anim_stop(void) {}
#define FM1_TICKS_PER_US 1u
static uint32_t host_ticks, host_pressed, host_notes;
static int32_t host_enc[7];
static uint32_t fm1_ticks(void) { return host_ticks; }
static uint32_t fm1_input_edges(int x) { uint32_t p = host_pressed; (void)x; host_pressed = 0; return p; }
static uint32_t fm1_input_note_edges(void) { uint32_t n = host_notes; host_notes = 0; return n; }
static int32_t fm1_enc_take(uint32_t e) { int32_t s = host_enc[e % 7u]; host_enc[e % 7u] = 0; return s; }
static void fm1_wdt_feed(void) {}
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static uint16_t host_screen[240 * 240];
static uint32_t host_dirty;
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t v = (uint16_t)((c >> 8) | (c << 8));
    host_dirty = 1;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            host_screen[(y + j) * 240u + x + i] = v;
}
static void lcd_sync(void) {}
static int host_bl = 1;
static void lcd_power(uint32_t s) { host_bl = s != 0u; host_dirty = 1; }
static void lcd_wake_now(void) { host_bl = 1; host_dirty = 1; }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    host_dirty = 1;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            host_screen[(y + j) * 240u + x + i] = p[j * w + i];
}
static struct { uint32_t stage; } felucca_dbg;
#define FELUCCA_FLASH 0
#define FELUCCA_VERSION "SHARD-EMU"
#include "../../firmware/src/gfx.c"
#include "../../firmware/src/panel.c"
#include "../../firmware/src/ui.c"
#include "../../firmware/src/menu_items.c"
#include "../../firmware/src/icons.c"
#include "../../firmware/src/ui_graph.c"
#include "../../firmware/src/ui_draw.c"
#include "../../firmware/src/ui_menu.c"
#include "../../firmware/src/ui_input.c"
#include "../../firmware/src/ui_layer.c"
#include "../../firmware/src/upreset.c"
#include "../../firmware/src/project.c"

/* main.c felucca_init, as tests/ui_test.c */
static void ui_power_on(void)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    memset(midi_lit_ms, 0, sizeof midi_lit_ms); midi_seen = 0;   /* (1.5.5: no MIDI IN light) */
    memset(&chain, 0, sizeof chain);
    chain_defaults(&chain_config);
    memset(&ui, 0, sizeof ui);
    page_over = 0;                                /* (no layer's knobs; roll_settle's test page gone) */
    cin_left = cin_total = 0;                     /* (no count-in) */
    memset(&nm, 0, sizeof nm);                    /* NAME closed, no project name */
    memset(&mom, 0, sizeof mom);                  /* MOMENTARY: no value held (the pool at power-on) */
    proj_name[0] = 0;
    proj_cur = PROJ_NO_SLOT;
    memset(&favorites, 0, sizeof favorites);
    ml_off();                                     /* (MIDI LEARN off, nothing learned) */
    memset(&undo, 0, sizeof undo);
    fop_op = fop_stg = 0;                         /* (FM6's operator pages: OP 1, stage 1, as at power-on) */
    memset(pat_last, 0, sizeof pat_last);
    memset(pat_from, 0, sizeof pat_from);                /* (the header's slot letter: none) */
    memset(proj_slot, 0, sizeof proj_slot);
    memset(up_bank, 0, sizeof up_bank);
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    undo_depth++;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        track_defaults(t);
        set_engine_of(t, TRK_DEF[i][0]);
        apply_preset_to(t, TRK_DEF[i][1]);
        t->engine = t->eng_req;
        track_defaults_steps(t);
        if (TRK_DEF[i][2])
            load_pat16(t, PATTERNS[TRK_DEF[i][2] - 1u].note, PATTERNS[TRK_DEF[i][2] - 1u].flags);
        pat_sig[i] = steps_sig(t);
        pat_last[i] = TRK_DEF[i][2];
    }
    undo_depth--;
    song.sel = 0;
    ui.home = 1;
    ui.force = 1;
    panel = PANEL_DEFAULT;
    settings.palette = UI_GREY_INDEX;
    palette_set(UI_GREY_INDEX);
    transport_req = 0;
    panic_req = 0;
    mi_r = mi_w = midi_in_overflow = 0;
    memset(midi_sel_on, 0, sizeof midi_sel_on);
    memset(midi_ch, 0, sizeof midi_ch);
    memset(midi_owners, 0, sizeof midi_owners);
    memset(midi_bend_q8, 0, sizeof midi_bend_q8);
    memset(midi_bend_target, 0, sizeof midi_bend_target);
    memset(kb_chn, 0, sizeof kb_chn);                   /* chord.c: no key, MIDI chord or last chord */
    memset(mchord, 0, sizeof mchord);
    memset(chord_last, 0, sizeof chord_last);
    perf_held = perf_act = kb_layer = perf_mask = 0;      /* the FX layer: nothing held */
    perf_latched = 0;
    memcpy((void *)perf_map, PF_DEF, sizeof PF_DEF);    /* .. its default key map (the settings' 0), no key's effect */
    perf_remap = 0;
    memset(kb_fx, 0, sizeof kb_fx);
    perf_kill = 0;
    perf_mask = 1u << panel.btn[B_FX];                  /* (ui_input sets them every frame) */
    kb_mask = layer_bits();
    perf_solo = 0;
    memset(&lys, 0, sizeof lys);
    perf_k[0] = perf_k[1] = perf_k[2] = perf_k[3] = 0;
    settings_hold = HOLD_DEF;
    fm1_in.buttons = fm1_in.notes = 0;
    memset(&motion, 0, sizeof motion);                  /* (main.c: no motion at power-on) */
    memset(motion_active, 0, sizeof motion_active);
    memset(motion_locked, 0, sizeof motion_locked);
    motion_base_valid = motion_full = 0;
    memset(snd_said, 0, sizeof snd_said);               /* (.bss at power-on: nothing said yet) */
    memset(&scrn, 0, sizeof scrn);                        /* MENU > SCREEN OFF: on, idle from now (main.c) */
    scrn.idle = fm1_ms;
    kb_asleep = 0;
    panel_moved = 0;
}
/* card slot drawn with MOTION's mark (ui_draw.c card_mot: its icon, or a long label in the theme colour) */
static int card_motion(uint32_t slot) { return (card_mot >> slot) & 1u; }



/* ------------------------------------------------------------ exports --- */
static int32_t emu_out[2u * 128u];
static uint32_t emu_held;                       /* buttons held (labels) */

EXPORT(emu_init) void emu_init(void)
{
    uint32_t k;
    for (k = 0; k < SMP_USER_SLOTS; k++)
        smp_user_scan(k);
    led_pos_init();
    ui_power_on();
    song.master_q12 = 4096;
    song.batt_raw = 600;                        /* a full battery */                     /* the MASTER pot full up (master.c reads the ADC) */
    ui.force = 1;
}
/* one UI frame: 16 ms */
EXPORT(emu_frame) void emu_frame(void)
{
    host_ticks += 16000u;
    fm1_ms += 16u;
    ui_input();
    ui_leds();
    ui_draw();
}
/* n (<= 128, a multiple of CTL) stereo frames into emu_out, Q15 int32 interleaved */
EXPORT(emu_render) int32_t *emu_render(uint32_t n)
{
    uint32_t b, i;
    for (b = 0; b < n; b += CTL) {
        int32_t *o = emu_out + 2u * b;
        mix_block(o, CTL);
        if (!scope_mix)
            for (i = 1; i < CTL; i += 2u)
                scope_buf[scope_w++ & (SCOPE_N - 1u)] = (int16_t)o[2u * i];
        click_render(o, CTL);
    }
    return emu_out;
}
EXPORT(emu_ctl) uint32_t emu_ctl(void) { return CTL; }
EXPORT(emu_screen) uint16_t *emu_screen(void) { return host_screen; }
EXPORT(emu_dirty) uint32_t emu_dirty(void) { uint32_t d = host_dirty; host_dirty = 0; return d | (uint32_t)(!host_bl) << 1; }
EXPORT(emu_button) void emu_button(uint32_t label, uint32_t down)
{
    uint32_t m;
    if (label >= NB)
        return;
    m = 1u << panel.btn[label];
    if (down) {
        fm1_in.buttons |= m;
        host_pressed |= m;
        emu_held |= 1u << label;
    } else {
        fm1_in.buttons &= ~m;
        emu_held &= ~(1u << label);
    }
}
EXPORT(emu_key) void emu_key(uint32_t k, uint32_t down)
{
    if (k >= 27u)
        return;
    if (down) {
        fm1_in.notes |= 1u << k;
        host_notes |= 1u << k;
    } else
        fm1_in.notes &= ~(1u << k);
}
EXPORT(emu_knob) void emu_knob(uint32_t role, int32_t steps)
{
    if (role < NE)
        host_enc[panel.enc[role]] += steps * panel.dir[role];
}
/* LED of id (0..13 matrix buttons, 14 + k keys, 41 PLAY's green): 3 lit, 2 mid, 1 dim glow / breath, 0 dark */
static int emu_led_at(uint8_t q, const uint8_t *l) { return q != 0xFF && ((l[q >> 3] >> (q & 7u)) & 1u); }
EXPORT(emu_led) uint32_t emu_led(uint32_t id)
{
    uint8_t q = id == 41u ? (uint8_t)LED_PLAY_GREEN : id < 41u ? led_pos[id] : 0xFF;
    if (emu_led_at(q, fm1_led)) return 3;
    if (emu_led_at(q, fm1_led_mid)) return 2;
    if (emu_led_at(q, fm1_led_breath)) return 4;
    if (emu_led_at(q, fm1_led_dim)) return 1;
    return 0;
}
EXPORT(emu_button_id) uint32_t emu_button_id(uint32_t label) { return label < NB ? panel.btn[label] : 0xFFu; }
EXPORT(emu_slot_ptr) uint8_t *emu_slot_ptr(uint32_t k) { return (uint8_t *)host_slots + k * SMP_USER_SIZE; }
EXPORT(emu_slot_size) uint32_t emu_slot_size(void) { return SMP_USER_SIZE; }
EXPORT(emu_slot_loaded) void emu_slot_loaded(uint32_t k)
{
    if (k < SMP_USER_SLOTS)
        smp_user_scan(k);
}
/* the selected track's engine, for the panel's caption (index into ENGINES) */
EXPORT(emu_engine_of) uint32_t emu_engine_of(uint32_t t) { return trk[t % NTRK].eng_req; }
EXPORT(emu_shard_index) uint32_t emu_shard_index(void)
{
    uint32_t e;
    for (e = 0; e < NENGINES; e++)
        if (ENGINES[e] == &ENG_SHARD) return e;
    return 0xFFu;
}
EXPORT(emu_set_engine) void emu_set_engine(uint32_t t, uint32_t e, uint32_t preset)
{
    track_t *tr = &trk[t % NTRK];
    set_engine_of(tr, e);
    apply_preset_to(tr, preset);
    ui.force = 1;
}
EXPORT(emu_select) void emu_select(uint32_t t) { song.sel = (uint8_t)(t % NTRK); ui.force = 1; }
EXPORT(emu_playing) uint32_t emu_playing(void) { return song.playing; }

/* the emulator's starting set: track 1 SHARD RECYCLE playing the break's slices in order (PATTERNS 11, SLICES),
 * track 2 SHARD CHOP KIT on CHOP (9), muted; track 4 DRUM on BEAT (12), muted; track 1 selected */
EXPORT(emu_demo) void emu_demo(void)
{
    uint32_t e = emu_shard_index();
    track_t *t;
    if (e >= NENGINES)
        return;
    t = &trk[0];
    set_engine_of(t, e); apply_preset_to(t, 0); t->engine = t->eng_req;
    load_pat16(t, PATTERNS[10].note, PATTERNS[10].flags); pat_sig[0] = steps_sig(t); pat_last[0] = 11;
    t = &trk[1];
    set_engine_of(t, e); apply_preset_to(t, 1); t->engine = t->eng_req;
    load_pat16(t, PATTERNS[8].note, PATTERNS[8].flags); pat_sig[1] = steps_sig(t); pat_last[1] = 9;
    t->p[P_MUTE] = 1;
    t = &trk[3];
    load_pat16(t, PATTERNS[11].note, PATTERNS[11].flags); pat_sig[3] = steps_sig(t); pat_last[3] = 12;
    t->p[P_MUTE] = 1;
    song.sel = 0;
    ui.force = 1;
}

/* the selected track's SRC (E0) when it plays sliced material (SHARD, SLICE): 0 BREAK, 1..3 USR1..3, 4 PIANO */
EXPORT(emu_set_src) uint32_t emu_set_src(uint32_t src)
{
    const engine_t *e = ENGINES[TSEL->eng_req % NENGINES];
    if ((e != &ENG_SHARD && e != &ENG_SLICE) || src >= SLC_NSRC)
        return 0;
    TSEL->p[P_E0] = (int16_t)src;
    ui.force = 1;
    return 1;
}

/* a source's tempo in 0.1 BPM: [0] found (shd_tempo_scan), [1] the kit's (0 AUTO) */
EXPORT(emu_tempo_found) uint32_t emu_tempo_found(uint32_t src) { return src < SHD_NSRC ? shd_det_bpm10[src] : 0u; }
EXPORT(emu_tempo_set) void emu_tempo_set(uint32_t src, uint32_t bpm)
{
    if (src < SHD_NSRC)
        shd_kit[src].bus.bpm = (uint8_t)(bpm >= SHD_BPM_MIN && bpm <= SHD_BPM_MAX ? bpm : 0u);
    ui.force = 1;
}
/* the selected track's DIV (SHARD's list: 0 "2" .. 6 "32", 7 AUTO, 8 MAN) when it plays SHARD; and BARS AUTO */
EXPORT(emu_set_div) uint32_t emu_set_div(uint32_t d)
{
    if (ENGINES[TSEL->eng_req % NENGINES] != &ENG_SHARD || d >= SHD_NDIV)
        return 0;
    TSEL->p[P_E1] = (int16_t)d;
    TSEL->p[P_E5] = 0;
    ui.force = 1;
    return 1;
}

/* a user slot's SENS (0..127): kept in its kit, its AUTO slices found again; the number found */
EXPORT(emu_sens_set) uint32_t emu_sens_set(uint32_t k, uint32_t sens)
{
    if (k >= SMP_USER_SLOTS)
        return 0;
    shd_kit[slc_src_of_slot(k)].bus.sens = (uint8_t)(sens > 127u ? 127u : sens);
    slc_rescan(k, shd_kit[slc_src_of_slot(k)].bus.sens);
    ui.force = 1;
    return slc_usr[k].len ? slc_usr[k].nauto : 0u;
}

/* user slot k's SHARD / SLICE SRC (USR4: 5, after PIANO) */
EXPORT(emu_src_of_slot) uint32_t emu_src_of_slot(uint32_t k) { return slc_src_of_slot(k); }
EXPORT(emu_slots) uint32_t emu_slots(void) { return SMP_USER_SLOTS; }

/* user slot k's samples (0 = empty), as the firmware has it now (an upload, a bounce) */
EXPORT(emu_slot_len) uint32_t emu_slot_len(uint32_t k) { return k < SMP_USER_SLOTS && usr_nz[k] ? slc_usr[k].len : 0u; }

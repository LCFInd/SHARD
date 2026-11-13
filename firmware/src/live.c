/* SPDX-License-Identifier: GPL-3.0-only */
/* LIVE (the sample mangler; the SCL button): the song's patterns launched by hand. The four PROJECT slots A..D hold
 * four patterns for each track (what GLO > SONG plays in its sections); LIVE plays one endless section whose lanes
 * the keys change while it runs (song_chain.c chain.live: lanes, chain.want, live_due).
 *   The keys (the LIVE page, tapped SCL; or SCL held / locked, the layer): the 16 white keys from F3 a 4 x 4 grid, four
 *   a track (T1: F3 G3 A3 B3 = A B C D, T2: C4 .. F4, T3: G4 .. C5, T4: D5 .. G5); black keys 1..4 (F#3 G#3 A#3 C#4)
 *   stop T1..T4 (a silent lane), 5..8 (D#4 F#4 G#4 A#4) put T1..T4 back on their own steps (OWN). Playing, a launch
 *   waits for the end of the track's loop (LAUNCH LOOP) or plays from its step 1 at once (LAUNCH NOW); stopped, it
 *   sets the lane PLAY starts with. The keys never sound here.
 *   KNOB 1 LAUNCH (LOOP, NOW). OCT+ (OWN): every track back on its own steps; stopped, LIVE off.
 *   PLAY with LIVE on loads the slots (live_load) and starts the lanes. A SONG started on GLO > SONG ends LIVE.
 * The page: a row a track, OWN and A..D: the lane playing inverted, a launch waiting blinks, an empty slot a dash. */
static uint32_t live_load(void);                /* project.c */
static uint8_t live_used;                       /* the PROJECT slots saved (bit s), read at most every LIVE_POLL ms */
static uint32_t live_used_ms;
#define LIVE_POLL 1500u

static void live_poll_used(int now)
{
    uint32_t s, u = 0;
    if (!now && live_used_ms && fm1_ms - live_used_ms < LIVE_POLL)
        return;
    if (transport_busy() && live_used_ms)
        return;                                 /* (stopped only: the slots' decode is the main loop's scratch) */
    for (s = 0; s < 4u; s++)
        if (project_used(s))
            u |= 1u << s;
    live_used = (uint8_t)u;
    live_used_ms = fm1_ms | 1u;
}
static int live_on_page(void)
{
    return !ui.home && !ui.menu && !ui.confirm && cur_page()->graph == GR_LIVE;
}
static int live_keys_on(void) { return live_on_page() && !ui.layer; }
/* the lane track k plays (or starts with) */
static uint32_t live_lane(uint32_t k)
{
    if (chain.running)
        return chain.lane[k];
    return chain.live ? chain.start[k] : CHAIN_OWN;
}
static uint32_t live_wait(uint32_t k) { return chain.running && chain.live ? chain.want[k] : CHAIN_NONE; }

/* track k to lane s (0..3 a slot, CHAIN_SILENT, CHAIN_OWN) */
static void live_launch(uint32_t k, uint32_t s)
{
    static const char *const EMPTY[4] = {"A EMPTY", "B EMPTY", "C EMPTY", "D EMPTY"};
    if (s < 4u && !((live_used >> s) & 1u)) {
        ui_message(EMPTY[s]);
        return;
    }
    if (chain.running && !chain.live) {
        ui_message("A SONG PLAYS");
        return;
    }
    if (!chain.live) {
        uint32_t i;
        for (i = 0; i < NTRK; i++)
            chain.start[i] = CHAIN_OWN;
        if (song.playing) {                     /* (no slots loaded under a running sequence: it starts at PLAY) */
            ui_message("STOP, THEN LAUNCH");
            return;
        }
        chain.live = 1;
    }
    if (chain.running)
        chain.want[k] = (uint8_t)s;             /* (seq.c: at the loop's end, or NOW at the next block) */
    else
        chain.start[k] = (uint8_t)s;
    ui.force = 1;
}
static void live_key(uint32_t k)
{
    uint32_t p = key_place(k);
    if (!key_black(k)) {
        if (p < 4u * NTRK)
            live_launch(p / 4u, p % 4u);
    } else if (p < NTRK) {
        live_launch(p, CHAIN_SILENT);
    } else if (p < 2u * NTRK) {
        live_launch(p - NTRK, CHAIN_OWN);
    }
}
static void live_keys(uint32_t notes)
{
    uint32_t k;
    live_poll_used(0);
    for (k = 0; k < 27u; k++)
        if ((notes >> k) & 1u)
            live_key(k);
}
/* PLAY with LIVE on: the slots loaded, then the transport */
static void live_play(void)
{
    live_used = (uint8_t)live_load();
    live_used_ms = fm1_ms | 1u;
    chain.armed = 1;
    transport_req = 1;
}
/* OCT+ (OWN): every track back on its own steps; stopped: LIVE off */
static void live_own(void)
{
    uint32_t k;
    if (!chain.live)
        return;
    if (!chain.running) {
        chain.live = 0;
        ui_message("LIVE OFF");
    } else {
        for (k = 0; k < NTRK; k++)
            if (chain.lane[k] != CHAIN_OWN)
                chain.want[k] = CHAIN_OWN;
    }
    ui.force = 1;
}
static void live_knob(uint32_t slot, int32_t steps)
{
    if (slot == 0u && steps)
        chain.now = (uint8_t)(steps > 0);
}
static int live_card(uint32_t c, const char **lb, char *val, const char **u, int32_t *r)
{
    *u = "";
    *r = -1;
    if (c == 0u) {
        *lb = "LAUNCH";
        str_cpy(val, chain.now ? "NOW" : "LOOP", 8);
        *r = chain.now ? 1000 : 0;
        return 1;
    }
    return 0;
}
/* the keys: the lanes playing lit, a waiting launch and the slots that can be launched breathe */
static uint32_t live_leds(uint32_t *br)
{
    uint32_t k, c = 0, b = 0;
    for (k = 0; k < 27u; k++) {
        uint32_t p = key_place(k), t, s, lane;
        if (!key_black(k)) {
            if (p >= 4u * NTRK)
                continue;
            t = p / 4u;
            s = p % 4u;
        } else if (p < 2u * NTRK) {
            t = p % NTRK;
            s = p < NTRK ? CHAIN_SILENT : CHAIN_OWN;
        } else {
            continue;
        }
        lane = live_lane(t);
        if (lane == s && (chain.live || s == CHAIN_OWN))
            c |= 1u << k;
        else if (live_wait(t) == s || (s < 4u ? (live_used >> s) & 1u : 1u))
            b |= 1u << k;
    }
    if (br)
        *br = b;
    return c;
}
static uint32_t live_sig(void)
{
    uint32_t k, h = chain.live * 3u + chain.running * 5u + chain.now * 7u + live_used * 11u;
    for (k = 0; k < NTRK; k++)
        h = h * 31u + live_lane(k) * 13u + live_wait(k);
    return h + (live_wait(0) != CHAIN_NONE || live_wait(1) != CHAIN_NONE || live_wait(2) != CHAIN_NONE ||
                live_wait(3) != CHAIN_NONE ? (fm1_ms / 250u) & 1u : 0u) * 977u;
}
static void live_box(int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, uint16_t c)   /* an outline t px */
{
    cv_rect(x, y, w, t, c);
    cv_rect(x, y + h - t, w, t, c);
    cv_rect(x, y + t, t, h - 2 * t, c);
    cv_rect(x + w - t, y + t, t, h - 2 * t, c);
}
/* the grid: T1..T4 down, OWN A B C D across; under it what LIVE is doing */
static void graph_live(void)
{
    static const char *const COL[5] = {"OWN", "A", "B", "C", "D"};
    static const char *const TRK[NTRK] = {"T1", "T2", "T3", "T4"};
    uint32_t k, c, blink = (fm1_ms / 250u) & 1u;
    live_poll_used(0);
    for (c = 0; c < 5u; c++)
        cv_text(50 + (int32_t)c * 38 + 16 - text_w(&AF_S, COL[c]) / 2, 3, &AF_S, COL[c], T_MID);
    for (k = 0; k < NTRK; k++) {
        uint32_t lane = live_lane(k), wait = live_wait(k);
        int32_t y = 19 + (int32_t)k * 20;
        cv_text(12, y + 1, &AF_M, TRK[k], lane == CHAIN_SILENT ? T_DIM : T_TEXT);
        for (c = 0; c < 5u; c++) {
            uint32_t s = c ? c - 1u : CHAIN_OWN;
            int32_t x = 50 + (int32_t)c * 38;
            int have = !c || ((live_used >> s) & 1u);
            int on = lane == s && (chain.live || !c);
            if (on)
                cv_rect(x, y, 32, 18, T_TEXT);
            else if (wait == s)
                live_box(x, y, 32, 18, blink ? 3 : 1, T_TEXT);   /* (a launch waiting: it pulses) */
            else if (have)
                live_box(x, y, 32, 18, 1, T_MID);
            if (c) {
                const char *t = have ? COL[c] : "-";
                cv_text_on(x + 16 - text_w(&AF_M, t) / 2, y + 1, &AF_M, t, on ? T_BG : wait == s ? T_TEXT : have ? T_MID : T_DIM,
                           on ? T_TEXT : cv_bg);
            }
        }
    }
    cv_text(12, 102, &AF_S,
            !live_used ? "SAVE PROJECTS A-D TO LAUNCH" : chain.running && !chain.live ? "A SONG PLAYS" :
            !chain.live ? "A KEY STARTS LIVE" : chain.running ? "PLAYING" : "PLAY STARTS THESE", T_MID);
}
